import argparse
import time
import datetime
import os
import sys

from tle_manager import TLEManager
from satellite import Satellite
from observer import Observer
from config_manager import ConfigManager
import web_server

def clear_screen():
    """Clears the console screen."""
    os.system('cls' if os.name == 'nt' else 'clear')

def main():
    """Main application logic."""

    # 1. Load Config
    # If python_tracker/config.yaml doesn't exist, try ../config.yaml or default
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
    parser.add_argument("--no-visible", action='store_true', default=cm.get('show_all_visible', False), help="Radio Mode: Ignore optical visibility constraints")
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

    # Start Web Server
    print("Starting Web UI on port 8080...")
    web_server.start_server_thread()

    print("Starting tracker... Press Ctrl+C to exit.")
    time.sleep(2)

    # --- Main Loop ---
    try:
        while True:
            t_now = datetime.datetime.now(datetime.timezone.utc)

            # 1. Update Sun Position (for terminator)
            sun_lat, sun_lon = observer.get_sun_position(t_now)

            visible_sats_display = [] # For Terminal
            web_sats_data = []        # For Web API

            for sat in satellites:
                # Update Satellite State
                sat.update_position(observer, t_now, args.trail_mins)

                # Filter Logic
                is_above_horizon = sat.el >= args.minel

                # Check for optical visibility if NOT in radio mode
                # If --no-visible is set, we skip optical check
                is_optically_valid = True
                if not args.no_visible:
                    is_optically_valid = (sat.visibility == "YES") # Only show Visual passes

                # Combine Filters
                if is_above_horizon and is_optically_valid:
                    # Add to lists
                    web_data = {
                        "id": sat.norad_id,
                        "n": sat.name,
                        "lat": sat.lat,
                        "lon": sat.lon,
                        "a": sat.az,
                        "e": sat.el,
                        "v": sat.visibility,
                        "next": sat.next_event, # TODO: Implement next event logic
                        "apo": sat.alt_km, # Using Alt as proxy for apogee/footprint calculation in JS
                        "trail": sat.trail
                    }
                    web_sats_data.append(web_data)

                    visible_sats_display.append({
                        'name': sat.name,
                        'az': sat.az,
                        'el': sat.el,
                        'range': sat.range,
                        'vis': sat.visibility
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

            # Terminal Output
            clear_screen()
            mode_str = "RADIO MODE (All Visible)" if args.no_visible else "OPTICAL MODE (Sunlit Only)"
            header = f"Observer: {args.lat:.2f}, {args.lon:.2f} | {mode_str} | {len(visible_sats_display)}/{len(satellites)} | {t_now.strftime('%H:%M:%S UTC')}"
            print(header)
            print("-" * len(header))

            print(f"{'Name':<25} {'Azimuth':>10} {'Elevation':>12} {'Range (km)':>15} {'Vis':>5}")
            print(f"{'='*25} {'='*10} {'='*12} {'='*15} {'='*5}")

            if not visible_sats_display:
                print("No satellites matching criteria.")
            else:
                for s in visible_sats_display:
                    print(f"{s['name']:<25} {s['az']:10.2f} {s['el']:12.2f} {s['range']:15.2f} {s['vis']:>5}")

            print("\n" + ("-" * len(header)))
            print(f"Web UI running at http://localhost:8080")
            print("Press Ctrl+C to exit.")

            time.sleep(1)

    except KeyboardInterrupt:
        print("\nTracker stopped by user.")
        sys.exit(0)
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}", file=sys.stderr)
        # sys.exit(1) # Keep running to see error?
        raise

if __name__ == "__main__":
    main()
