import argparse
import time
import datetime
import os
import sys

from tle_manager import TLEManager
from satellite import Satellite
from observer import Observer

def clear_screen():
    """Clears the console screen."""
    os.system('cls' if os.name == 'nt' else 'clear')

def main():
    """Main application logic."""
    parser = argparse.ArgumentParser(description="A simple Python satellite tracker using Skyfield.")
    parser.add_argument("--lat", type=float, default=39.5478, help="Observer Latitude (dd)")
    parser.add_argument("--lon", type=float, default=-76.0916, help="Observer Longitude (dd)")
    parser.add_argument("--alt", type=float, default=0.1, help="Observer Altitude (km)")
    parser.add_argument("--groupsel", type=str, default="amateur,weather,stations", help="Comma-separated Celestrak group names")
    parser.add_argument("--minel", type=float, default=0.0, help="Minimum elevation filter (degrees)")

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
    print("Starting tracker... Press Ctrl+C to exit.")
    time.sleep(2)

    # --- Main Loop ---
    try:
        while True:
            clear_screen()
            now = datetime.datetime.now(datetime.timezone.utc)

            visible_sats = []
            for sat in satellites:
                az, el, r = observer.calculate_look_angle(sat, now)
                if el is not None and el >= args.minel:
                    visible_sats.append({
                        'name': sat.name,
                        'az': az,
                        'el': el,
                        'range': r,
                    })

            visible_sats.sort(key=lambda s: s['el'], reverse=True)

            header = f"Observer: Lat {args.lat:.2f}, Lon {args.lon:.2f} | Visible (> {args.minel}°): {len(visible_sats)}/{len(satellites)} | {now.strftime('%Y-%m-%d %H:%M:%S UTC')}"
            print(header)
            print("-" * len(header))

            print(f"{'Name':<25} {'Azimuth':>10} {'Elevation':>12} {'Range (km)':>15}")
            print(f"{'='*25} {'='*10} {'='*12} {'='*15}")

            if not visible_sats:
                print("No satellites currently visible above the horizon.")
            else:
                for sat in visible_sats:
                    print(f"{sat['name']:<25} {sat['az']:10.2f} {sat['el']:12.2f} {sat['range']:15.2f}")

            print("\n" + ("-" * len(header)))
            print("Press Ctrl+C to exit.")

            time.sleep(1)

    except KeyboardInterrupt:
        print("\nTracker stopped by user.")
        sys.exit(0)
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
