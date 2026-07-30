"""Entry point for the Python satellite tracker.

Parses the command line (mirroring the C++ flags, including the --hpop
High-Precision Orbit Propagator options), loads TLEs (live Celestrak or
historical Space-Track), builds the Observer and Satellite objects - optionally
with the HPOP backend - and runs the real-time loop that propagates every
satellite, classifies visibility, and feeds the terminal display and the web,
text, and physics servers. Functional twin of the C++ src/main.cpp.
"""
import argparse
import time
import datetime
import os
import sys
import subprocess
import concurrent.futures

# Platform-specific imports for keyboard handling
if sys.platform != 'win32':
    import select
    import termios
    import tty
    HAS_TERMIOS = True
else:
    HAS_TERMIOS = False

# Support two invocation styles:
#   * `python3 main.py`               (cwd = python_tracker/, flat imports)
#   * `python3 -m python_tracker.main` (cwd = repo root, package imports —
#                                       this is what the launchers and ve-ide use)
try:
    from .tle_manager import TLEManager
    from .satellite import Satellite
    from .observer import Observer
    from .config_manager import ConfigManager
    from . import hpop
    from . import web_server
    from . import text_server
    from . import physics_server
except ImportError:
    from tle_manager import TLEManager
    from satellite import Satellite
    from observer import Observer
    from config_manager import ConfigManager
    import hpop
    import web_server
    import text_server
    import physics_server

def clear_screen():
    """Clears the console screen."""
    if os.name == 'nt':
        subprocess.run(['cls'], shell=True, check=False)
    else:
        subprocess.run(['clear'], check=False)

class KeyPoller:
    """Non-blocking single-key reader.

    Degrades to a silent no-op when stdin is not a real terminal (e.g. when
    launched from ve-ide via QProcess, a systemd unit, or piped input). This
    keeps the tracker usable in headless / GUI-launched contexts — the user
    just can't press 'q'; they stop the process the way it was started.
    """
    def __init__(self):
        self._active = False

    def __enter__(self):
        if HAS_TERMIOS and sys.stdin.isatty():
            try:
                self.old_settings = termios.tcgetattr(sys.stdin)
                tty.setcbreak(sys.stdin.fileno())
                self._active = True
            except (termios.error, OSError):
                self._active = False
        return self

    def __exit__(self, type, value, traceback):
        if self._active:
            try:
                termios.tcsetattr(sys.stdin, termios.TCSADRAIN, self.old_settings)
            except (termios.error, OSError):
                pass

    def poll(self):
        if not self._active:
            return None
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

    parser = argparse.ArgumentParser(
        description="A simple Python satellite tracker using Skyfield.",
        epilog="""Network Ports:
  Port 8080   - Web Dashboard / Mission Planner UI (HTTP)
  Port 12345  - Terminal Mirror Server (HTTP text display)
  Port 12346  - Physics Stream Server (TCP, for tracking_client.py)

Configuration is loaded from config.yaml by default.""",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--lat", type=float, default=cm.get('lat', 39.5478), help="Observer Latitude (dd)")
    parser.add_argument("--lon", type=float, default=cm.get('lon', -76.0916), help="Observer Longitude (dd)")
    parser.add_argument("--alt", type=float, default=cm.get('alt', 0.1), help="Observer Altitude (km)")
    parser.add_argument("--groupsel", type=str, default=cm.get('group_selection', "active"), help="Comma-separated Celestrak group names")
    parser.add_argument("--minel", type=float, default=cm.get('min_el', 0.0), help="Minimum elevation filter (degrees)")
    parser.add_argument("--maxsats", type=int, default=cm.get('max_sats', 100), help="Maximum satellites to display")
    # visible_only: False = show ALL satellites, True = optical mode (visible only)
    # Legacy: --no-visible flag or show_all_visible config inverts this
    default_visible_only = cm.get('visible_only', not cm.get('show_all_visible', False))
    parser.add_argument("--visible", action='store_true', default=default_visible_only, help="Optical mode: show only visible satellites")
    parser.add_argument("--no-visible", action='store_true', help="Show ALL satellites (ignore visibility filter)")
    parser.add_argument("--trail_mins", type=int, default=cm.get('trail_length_mins', 5), help="Trail length in minutes")
    parser.add_argument("--maxapo", "--map_apo", type=float, default=cm.get('max_apo', -1), dest='maxapo', help="Maximum apogee filter (km). Satellites above this are excluded. -1 disables.")
    parser.add_argument("--deltaT", type=float, default=cm.get('delta_t', 1.0), dest='delta_t', help="Time increment between calculations (0.001-60 seconds, default 1)")
    parser.add_argument("--satsel", type=str, default=cm.get('sat_selection', ""), help="Comma-separated satellite names (overrides --groupsel)")
    parser.add_argument("--time", type=str, default=None, help='Simulate time: "YYYY-MM-DD HH:MM:SS" (UTC). Times more than 24h from real-now fetch historical TLEs from Space-Track.')
    parser.add_argument("--hpop", action='store_true', help="Use the High-Precision Orbit Propagator (EGM96 gravity + Sun/Moon + drag + SRP) instead of SGP4.")
    parser.add_argument("--hpop-degree", type=int, default=10, dest='hpop_degree', help="HPOP geopotential degree/order (1-20, default 10)")
    parser.add_argument("--no-drag", action='store_true', help="HPOP: disable atmospheric drag")
    parser.add_argument("--no-srp", action='store_true', help="HPOP: disable solar radiation pressure")
    parser.add_argument("--no-thirdbody", action='store_true', help="HPOP: disable Sun/Moon third-body perturbations")

    args = parser.parse_args()

    # Build HPOP options (None disables the backend; satellites then use Skyfield/SGP4).
    hpop_opts = None
    if args.hpop:
        if not hpop.available():
            print("[ERROR] --hpop requested but the ve_hpop module is unavailable.\n"
                  + hpop.import_hint(), file=sys.stderr)
            sys.exit(2)
        deg = max(1, min(20, args.hpop_degree))
        hpop_opts = dict(degree=deg, drag=not args.no_drag, srp=not args.no_srp,
                         thirdbody=not args.no_thirdbody)

    # Parse --time into an aware UTC datetime (or None for real-time).
    sim_time_utc = None
    if args.time:
        try:
            sim_time_utc = datetime.datetime.strptime(args.time, "%Y-%m-%d %H:%M:%S") \
                .replace(tzinfo=datetime.timezone.utc)
            print(f"[TIME] Simulating UTC time: {sim_time_utc.isoformat()}")
        except ValueError:
            print(f'[ERROR] --time must be "YYYY-MM-DD HH:MM:SS" (got {args.time!r})', file=sys.stderr)
            sys.exit(2)

    # Validate delta_t range
    if args.delta_t < 0.001 or args.delta_t > 60.0:
        print(f"[WARN] --deltaT must be between 0.001 and 60 seconds. Using default (1.0).")
        args.delta_t = 1.0

    # Handle visible_only logic: --no-visible overrides --visible
    if args.no_visible:
        args.visible_only = False
    else:
        args.visible_only = args.visible

    # --- Initialization ---
    observer = Observer(args.lat, args.lon, args.alt)
    tle_manager = TLEManager()

    # Decide between live Celestrak and historical Space-Track gp_history based on
    # how far the simulated time is from real-now. Matches the C++ gate of +/- 24 h.
    use_historical = False
    if sim_time_utc is not None:
        gap = abs((datetime.datetime.now(datetime.timezone.utc) - sim_time_utc).total_seconds())
        use_historical = gap > 86400.0

    hist_window_days = 10

    if args.satsel:
        print(f"Loading specific satellites: {args.satsel}"
              + (" [HISTORICAL]" if use_historical else "") + "...")
        if use_historical:
            tles = tle_manager.load_specific_sats_for_date(args.satsel, sim_time_utc, hist_window_days)
        else:
            tles = tle_manager.load_specific_sats(args.satsel)
    else:
        print(f"Loading TLEs for group(s): {args.groupsel}"
              + (" [HISTORICAL]" if use_historical else "") + "...")
        if use_historical:
            tles = tle_manager.load_groups_for_date(args.groupsel, sim_time_utc, hist_window_days)
        else:
            tles = tle_manager.load_groups(args.groupsel)

    if not tles:
        print("Error: Could not load any TLE data. Check your network connection, group names, or Space-Track credentials.", file=sys.stderr)
        sys.exit(1)

    satellites = [Satellite(tle, hpop_opts=hpop_opts) for tle in tles]
    print(f"Successfully loaded {len(satellites)} satellites.")
    if hpop_opts is not None:
        n_hpop = sum(1 for s in satellites if s.hpop is not None)
        print(f"[HPOP] High-precision propagator active for {n_hpop} satellites "
              f"(geopotential {hpop_opts['degree']}x{hpop_opts['degree']}, "
              f"drag={hpop_opts['drag']}, srp={hpop_opts['srp']}, 3-body={hpop_opts['thirdbody']}).")

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

    # Start Physics Server (TCP stream for tracking_client.py)
    print("Starting Physics Stream on port 12346...")
    try:
        phys_server = physics_server.PhysicsServer(12346)
        phys_server.start()
    except Exception as e:
        print(f"Failed to start PhysicsServer: {e}")
        phys_server = None

    print("Starting tracker... Press 'q' to quit.")
    time.sleep(2)

    # Decoupled clock: when simulating, physics time = sim_time_utc + elapsed wall-clock
    # since program start. This mirrors the C++ physics_epoch + (Clock::now() - start) logic.
    wall_start = datetime.datetime.now(datetime.timezone.utc)

    # --- Main Loop ---
    try:
        with KeyPoller() as key_poller:
            while True:
                # Check for input
                char = key_poller.poll()
                if char is not None:
                    if char.lower() == 'q':
                        break # Exit loop to handle save prompt

                if sim_time_utc is not None:
                    elapsed = datetime.datetime.now(datetime.timezone.utc) - wall_start
                    t_now = sim_time_utc + elapsed
                else:
                    t_now = datetime.datetime.now(datetime.timezone.utc)
                # Convert to Skyfield Time once for efficiency
                t_now_ts = observer.ts.from_datetime(t_now)

                # 1. Update Sun Position (for terminator)
                sun_lat, sun_lon = observer.get_sun_position(t_now_ts)

                visible_sats_display = [] # For Terminal
                web_sats_data = []        # For Web API

                for sat in satellites:
                    # Skip decayed satellites (apogee < 80km) - matches C++ behavior
                    if sat.is_decayed():
                        continue

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
                    # When visible_only=False: Show ALL satellites (color by elevation/visibility in UI)
                    # When visible_only=True: Only show optically visible satellites above min_el
                    should_display = False

                    if not args.visible_only:
                        # Radio/Show All Mode: Display every satellite in the group
                        should_display = True
                    else:
                        # Optical Mode: Must be above min_el AND visibly illuminated
                        is_above_horizon = sat.el >= args.minel
                        is_optically_valid = (sat.visibility == "YES")
                        should_display = is_above_horizon and is_optically_valid

                    # Apply max apogee filter (if enabled)
                    if should_display and args.maxapo > 0 and sat.apogee > args.maxapo:
                        should_display = False

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
                            "apo": sat.apogee,
                            "f": sat.flare_status,
                            "trail": sat.trail
                        }
                        web_sats_data.append(web_data)

                        visible_sats_display.append({
                            'name': sat.name,
                            'az': sat.az,
                            'el': sat.el,
                            'range': sat.range,
                            'range_rate': sat.range_rate,
                            'vis': sat.visibility,
                            'next': sat.next_event,
                            'norad_id': sat.norad_id,
                            'lat': sat.lat,
                            'lon': sat.lon,
                            'apogee': sat.apogee,
                            'flare_status': sat.flare_status
                        })

                # Sort for display (by elevation, highest first)
                visible_sats_display.sort(key=lambda s: s['el'], reverse=True)
                web_sats_data.sort(key=lambda s: s['e'], reverse=True)

                # Apply max_sats limit
                # Web data: No limit when visible_only=False (radio mode) - show ALL satellites
                # Terminal display: Always apply max_sats limit for readability
                if args.visible_only and args.maxsats > 0 and len(web_sats_data) > args.maxsats:
                    web_sats_data = web_sats_data[:args.maxsats]
                if args.maxsats > 0 and len(visible_sats_display) > args.maxsats:
                    visible_sats_display = visible_sats_display[:args.maxsats]

                # Update Shared Web State (thread-safe)
                web_server.update_tracker_state(
                    config={
                        'lat': args.lat, 'lon': args.lon, 'min_el': args.minel,
                        'max_apo': args.maxapo, 'show_all': not args.visible_only,
                        'groups': args.groupsel,
                        'sun_lat': sun_lat, 'sun_lon': sun_lon
                    },
                    satellites=web_sats_data
                )

                # --- Build Output ---
                mode_str = "SHOW ALL (Radio Mode)" if not args.visible_only else "OPTICAL MODE (Sunlit Only)"

                output_lines = []
                header = f"Observer: {args.lat:.2f}, {args.lon:.2f} | {mode_str} | {len(visible_sats_display)}/{len(satellites)} | {t_now.strftime('%H:%M:%S UTC')}"
                output_lines.append(header)
                output_lines.append("-" * len(header))
                # Match C++ format: NAME AZ EL RANGE RR VIS NEXT_EVENT NORAD LAT LON APOGEE FLARE
                col_header = f"{'NAME':<15} {'AZ':>8} {'EL':>8} {'RANGE':>10} {'RR':>8} {'VIS':>5} {'NEXT EVENT':<12} {'NORAD':>8} {'LAT':>10} {'LON':>10} {'APOGEE':>10} {'FLARE':>6}"
                output_lines.append(col_header)
                output_lines.append("-" * len(col_header))

                if not visible_sats_display:
                    output_lines.append("No satellites matching criteria.")
                else:
                    limit_console = 30
                    limit_text = 200

                    # Console Print
                    clear_screen()
                    for line in output_lines: print(line)

                    for i, s in enumerate(visible_sats_display):
                        # Truncate name to match C++ format (14 chars + potential F indicator)
                        name_str = s['name'][:14]
                        if s['flare_status'] > 0:
                            name_str += " F"

                        # Match C++ format exactly
                        line = f"{name_str:<15} {s['az']:8.1f} {s['el']:8.1f} {s['range']:10.1f} {s['range_rate']:8.3f} {s['vis']:>5} {s['next']:<12} {s['norad_id']:8d} {s['lat']:10.4f} {s['lon']:10.4f} {s['apogee']:10.1f} {s['flare_status']:6d}"

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
                print(f"Physics Stream running at localhost:12346 (TCP)")
                print("Press 'q' to quit.")

                # Update TextServer
                if txt_server: txt_server.update_data("\n".join(output_lines))

                # Update PhysicsServer with C++-compatible frame format
                if phys_server:
                    physics_lines = []
                    physics_lines.append("VISIBLE EPHEMERIS v12.65-CODE-ONLY")
                    physics_lines.append(t_now.strftime("%Y-%m-%d %H:%M:%S.0") + " LOC")
                    physics_lines.append(f"OBS: {args.lat}, {args.lon} | SHOWN: {len(visible_sats_display)}")
                    physics_lines.append("")
                    # Header matching C++ format
                    physics_lines.append(f"{'NAME':<15} {'AZ':>8} {'EL':>8} {'RANGE':>10} {'RR(km/s)':>8} {'VIS':>5} {'NEXT EVENT':<12} {'NORAD':>8} {'LAT':>10} {'LON':>10} {'APOGEE':>10} {'FLARE':>6}")
                    physics_lines.append("-" * 118)
                    for s in visible_sats_display:
                        name_str = s['name'][:14]
                        if s['flare_status'] > 0:
                            name_str += " F"
                        vis_str = "VIS" if s['vis'] == "YES" else ("DAY" if s['vis'] == "DAY" else "ECL")
                        if s['el'] < 0:
                            vis_str = "HOR"
                        physics_lines.append(f"{name_str:<15} {s['az']:8.1f} {s['el']:8.1f} {s['range']:10.1f} {s['range_rate']:8.3f} {vis_str:<5} {s['next']:<12} {s['norad_id']:8d} {s['lat']:10.4f} {s['lon']:10.4f} {s['apogee']:10.1f} {s['flare_status']:6d}")
                    phys_server.update_data("\n".join(physics_lines))

                time.sleep(args.delta_t)

        # --- Shutdown / Save Prompt ---
        print("\nStopping tracker...")
        if not sys.stdin.isatty():
            # No terminal (e.g. launched from ve-ide via QProcess): skip the
            # prompt — leave the on-disk config untouched.
            print("(non-interactive stdin — skipping save prompt)")
            if txt_server: txt_server.stop()
            if phys_server: phys_server.stop()
            executor.shutdown(wait=True)
            return
        while True:
            response = input("Save configuration to config.yaml? (y/n): ").strip().lower()
            if response in ['y', 'yes']:
                new_config = {
                    'lat': args.lat,
                    'lon': args.lon,
                    'alt': args.alt,
                    'min_el': args.minel,
                    'max_sats': args.maxsats,
                    'max_apo': args.maxapo,
                    'visible_only': args.visible_only,
                    'group_selection': args.groupsel,
                    'trail_length_mins': args.trail_mins,
                    'delta_t': args.delta_t
                }
                cm.save(new_config)
                break
            elif response in ['n', 'no']:
                print("Configuration not saved.")
                break

        if txt_server: txt_server.stop()
        if phys_server: phys_server.stop()
        executor.shutdown(wait=True)

    except KeyboardInterrupt:
        print("\nTracker stopped by user.")
        if txt_server: txt_server.stop()
        if 'phys_server' in locals() and phys_server: phys_server.stop()
        if 'executor' in locals(): executor.shutdown(wait=True)
        sys.exit(0)
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}", file=sys.stderr)
        if 'txt_server' in locals() and txt_server: txt_server.stop()
        if 'phys_server' in locals() and phys_server: phys_server.stop()
        if 'executor' in locals(): executor.shutdown(wait=True)
        raise

if __name__ == "__main__":
    main()
