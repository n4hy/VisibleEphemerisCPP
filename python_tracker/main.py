import argparse
import time
import datetime
import os
import sys
import select
import termios
import tty
import concurrent.futures

from tle_manager import TLEManager
from satellite import Satellite
from observer import Observer
from config_manager import ConfigManager
import web_server
import text_server

def clear_screen():
    """Clears the console screen."""
    os.system('cls' if os.name == 'nt' else 'clear')

class KeyPoller:
    def __enter__(self):
        self.old_settings = termios.tcgetattr(sys.stdin)
        tty.setcbreak(sys.stdin.fileno())
        return self

    def __exit__(self, type, value, traceback):
        termios.tcsetattr(sys.stdin, termios.TCSADRAIN, self.old_settings)

    def poll(self):
        if select.select([sys.stdin], [], [], 0) == ([sys.stdin], [], []):
            return sys.stdin.read(1)
        return None

def main():
    """Main application logic."""

    # 1. Load Config
    config_path = "python_tracker/config.yaml"
    if not os.path.exists(config_path):
        config_path = "config.yaml"

    cm = ConfigManager(config_path)

    parser = argparse.ArgumentParser(description="A simple Python satellite tracker using Skyfield.")
    parser.add_argument("--lat", type=float, default=cm.get('lat', 39.5478), help="Observer Latitude (dd)")
    parser.add_argument("--lon", type=float, default=cm.get('lon', -76.0916), help="Observer Longitude (dd)")
    parser.add_argument("--alt", type=float, default=cm.get('alt', 0.1), help="Observer Altitude (km)")
    parser.add_argument("--groupsel", type=str, default=cm.get('group_selection', "active"), help="Comma-separated Celestrak group names")
    parser.add_argument("--minel", type=float, default=cm.get('min_el', 0.0), help="Minimum elevation filter (degrees)")
    parser.add_argument("--no-visible", action='store_true', default=cm.get('show_all_visible', False), help="Show ALL satellites (ignore filters)")
    parser.add_argument("--trail_mins", type=int, default=cm.get('trail_length_mins', 5), help="Trail length in minutes")

    args = parser.parse_args()

    # --- Initialization ---
    observer = Observer(args.lat, args.lon, args.alt)
    tle_manager = TLEManager()

    print(f"Loading TLEs for group(s): {args.groupsel}...")
    tles = tle_manager.load_groups(args.groupsel)

    if not tles:
        print("Error: Could not load any TLE data. Check your network connection or group names.", file=sys.stderr)
        sys.exit(1)

    satellites = [Satellite(tle) for tle in tles]
    print(f"Successfully loaded {len(satellites)} satellites.")

    # Thread Pool for Math
    executor = concurrent.futures.ThreadPoolExecutor(max_workers=4)

    # Start Web Server
    print("Starting Web UI on port 8080...")
    web_server.start_server_thread()

    # Start Text Server
    print("Starting Text Mirror on port 12345...")
    try:
        txt_server = text_server.TextServer(12345)
        txt_server.start()
    except Exception as e:
        print(f"Failed to start TextServer: {e}")
        txt_server = None

    print("Starting tracker... Press 'q' to quit.")
    time.sleep(2)

    # --- Main Loop ---
    try:
        with KeyPoller() as key_poller:
            while True:
                # Check for input
                char = key_poller.poll()
                if char is not None:
                    if char.lower() == 'q':
                        break # Exit loop to handle save prompt

                t_now = datetime.datetime.now(datetime.timezone.utc)
                # Convert to Skyfield Time once for efficiency
                t_now_ts = observer.ts.from_datetime(t_now)

                # 1. Update Sun Position (for terminator)
                sun_lat, sun_lon = observer.get_sun_position(t_now_ts)

                visible_sats_display = [] # For Terminal
                web_sats_data = []        # For Web API

                for sat in satellites:
                    # Update Satellite State
                    sat.update_position(observer, t_now_ts, args.trail_mins)

                    # Pass Prediction Logic
                    # Check if passes are stale (older than 24h) or empty
                    needs_calc = False
                    if not sat.passes:
                        needs_calc = True
                    elif sat.last_pass_calc is not None:
                        # Fix: Ensure last_pass_calc is not treated as boolean directly to avoid Skyfield TypeError
                        age = (t_now - sat.last_pass_calc.utc_datetime()).total_seconds()
                        if age > 86400: # 24 hours
                            needs_calc = True

                    if needs_calc and not sat.is_computing:
                        sat.is_computing = True
                        # Submit to thread pool
                        executor.submit(sat.compute_passes, observer.location, t_now_ts, 1, args.minel).add_done_callback(
                            lambda future, s=sat: setattr(s, 'is_computing', False)
                        )

                    sat.next_event = sat.get_next_event_text(t_now_ts)

                    # Filter Logic
                    should_display = False

                    if args.no_visible:
                        # User Request: "REJECT ALL FILTERS... I want to see every bloody satellite"
                        should_display = True
                    else:
                        # Optical Mode: Must be above horizon AND visibly illuminated
                        is_above_horizon = sat.el >= args.minel
                        is_optically_valid = (sat.visibility == "YES")
                        should_display = is_above_horizon and is_optically_valid

                    if should_display:
                        # Add to lists
                        web_data = {
                            "id": sat.norad_id,
                            "n": sat.name,
                            "lat": sat.lat,
                            "lon": sat.lon,
                            "a": sat.az,
                            "e": sat.el,
                            "v": sat.visibility,
                            "next": sat.next_event,
                            "apo": sat.alt_km,
                            "trail": sat.trail
                        }
                        web_sats_data.append(web_data)

                        visible_sats_display.append({
                            'name': sat.name,
                            'az': sat.az,
                            'el': sat.el,
                            'range': sat.range,
                            'vis': sat.visibility,
                            'next': sat.next_event
                        })

                # Sort for display
                visible_sats_display.sort(key=lambda s: s['el'], reverse=True)

                # Update Shared Web State
                web_server.tracker_state['config'] = {
                    'lat': args.lat, 'lon': args.lon, 'min_el': args.minel,
                    'max_apo': -1, 'show_all': args.no_visible,
                    'groups': args.groupsel,
                    'sun_lat': sun_lat, 'sun_lon': sun_lon
                }
                web_server.tracker_state['satellites'] = web_sats_data

                # --- Build Output ---
                mode_str = "SHOW ALL (No Filter)" if args.no_visible else "OPTICAL MODE (Sunlit Only)"

                output_lines = []
                header = f"Observer: {args.lat:.2f}, {args.lon:.2f} | {mode_str} | {len(visible_sats_display)}/{len(satellites)} | {t_now.strftime('%H:%M:%S UTC')}"
                output_lines.append(header)
                output_lines.append("-" * len(header))
                output_lines.append(f"{'Name':<20} {'Az':>6} {'El':>6} {'Range':>10} {'Vis':>4} {'Next Event':>15}")
                output_lines.append(f"{'='*20} {'='*6} {'='*6} {'='*10} {'='*4} {'='*15}")

                if not visible_sats_display:
                    output_lines.append("No satellites matching criteria.")
                else:
                    limit_console = 30
                    limit_text = 200

                    # Console Print
                    clear_screen()
                    for line in output_lines: print(line)

                    for i, s in enumerate(visible_sats_display):
                        # Truncate name
                        name_str = (s['name'][:19] + '..') if len(s['name']) > 19 else s['name']
                        line = f"{name_str:<20} {s['az']:6.1f} {s['el']:6.1f} {s['range']:10.0f} {s['vis']:>4} {s['next']:>15}"

                        if i < limit_console:
                            print(line)
                        if i == limit_console:
                            print(f"... and {len(visible_sats_display)-limit_console} more ...")

                        # Add to TextServer buffer
                        if i < limit_text:
                            output_lines.append(line)

                    if len(visible_sats_display) > limit_text:
                        output_lines.append(f"... and {len(visible_sats_display)-limit_text} more ...")

                print("\n" + ("-" * len(header)))
                print(f"Web UI running at http://localhost:8080")
                print(f"Text Mirror running at http://localhost:12345")
                print("Press 'q' to quit.")

                # Update TextServer
                if txt_server: txt_server.update_data("\n".join(output_lines))

                time.sleep(1)

        # --- Shutdown / Save Prompt ---
        print("\nStopping tracker...")
        while True:
            response = input("Save configuration to config.yaml? (y/n): ").strip().lower()
            if response in ['y', 'yes']:
                new_config = {
                    'lat': args.lat,
                    'lon': args.lon,
                    'alt': args.alt,
                    'min_el': args.minel,
                    'show_all_visible': args.no_visible,
                    'group_selection': args.groupsel,
                    'trail_length_mins': args.trail_mins
                }
                cm.save(new_config)
                break
            elif response in ['n', 'no']:
                print("Configuration not saved.")
                break

        if txt_server: txt_server.stop()
        executor.shutdown(wait=False)

    except KeyboardInterrupt:
        print("\nTracker stopped by user.")
        if txt_server: txt_server.stop()
        if 'executor' in locals(): executor.shutdown(wait=False)
        sys.exit(0)
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}", file=sys.stderr)
        if 'txt_server' in locals() and txt_server: txt_server.stop()
        if 'executor' in locals(): executor.shutdown(wait=False)
        raise

if __name__ == "__main__":
    main()
