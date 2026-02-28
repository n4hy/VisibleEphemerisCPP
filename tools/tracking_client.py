#!/usr/bin/env python3
"""
TCP Client for VisibleEphemeris Physics Stream

Connects to the C++ tracker's raw TCP output (port 12346) and parses
satellite tracking data for display or further processing.

Usage:
    python3 tracking_client.py [--host HOST] [--port PORT] [--json] [--raw]

Examples:
    python3 tracking_client.py                    # Default: localhost:12346
    python3 tracking_client.py --host 192.168.1.5 # Remote host
    python3 tracking_client.py --json             # Output as JSON
    python3 tracking_client.py --raw              # Show raw frames
"""

import socket
import argparse
import sys
import json
import re
from dataclasses import dataclass, asdict
from typing import List, Optional
from datetime import datetime


@dataclass
class SatelliteData:
    """Parsed satellite tracking data."""
    name: str
    azimuth: float
    elevation: float
    range_km: float
    range_rate: float
    visibility: str
    next_event: str
    norad_id: int = 0
    latitude: float = 0.0
    longitude: float = 0.0
    apogee: float = 0.0
    flare_status: int = 0


@dataclass
class TrackingFrame:
    """Complete tracking frame with metadata and satellite list."""
    timestamp: str
    observer_lat: float
    observer_lon: float
    satellites_shown: int
    satellites: List[SatelliteData]
    raw_frame: str = ""


def parse_frame(raw_data: str) -> Optional[TrackingFrame]:
    """Parse a raw tracking frame into structured data."""
    lines = raw_data.strip().split('\n')
    if len(lines) < 5:
        return None

    # Parse header
    # Line 0: "VISIBLE EPHEMERIS v12.65-CODE-ONLY"
    # Line 1: "2026-02-25 03:23:12.7 LOC"
    # Line 2: "OBS: 39.5478, -76.0916 | SHOWN: 5"
    # Line 3: empty or header
    # Line 4: "NAME            AZ       EL      RANGE   RR(km/s) VIS   NEXT EVENT     NORAD        LAT        LON     APOGEE  FLARE"
    # Line 5: "----..."
    # Line 6+: data rows

    timestamp = ""
    observer_lat = 0.0
    observer_lon = 0.0
    satellites_shown = 0

    for i, line in enumerate(lines):
        if "LOC" in line or "UTC" in line:
            timestamp = line.strip()
        elif line.startswith("OBS:"):
            # Parse: "OBS: 39.5478, -76.0916 | SHOWN: 5"
            match = re.search(r'OBS:\s*([-\d.]+),\s*([-\d.]+).*SHOWN:\s*(\d+)', line)
            if match:
                observer_lat = float(match.group(1))
                observer_lon = float(match.group(2))
                satellites_shown = int(match.group(3))

    # Find data rows (after the dashed separator line)
    data_start = -1
    for i, line in enumerate(lines):
        if line.startswith('---') and len(line) > 20:
            data_start = i + 1
            break

    satellites = []
    if data_start > 0:
        for line in lines[data_start:]:
            sat = parse_satellite_line(line)
            if sat:
                satellites.append(sat)

    return TrackingFrame(
        timestamp=timestamp,
        observer_lat=observer_lat,
        observer_lon=observer_lon,
        satellites_shown=satellites_shown,
        satellites=satellites,
        raw_frame=raw_data
    )


def parse_satellite_line(line: str) -> Optional[SatelliteData]:
    """Parse a single satellite data line."""
    # Format: "%-15s %8.1f %8.1f %10.1f %8.3f %-5s %-12s %8d %10.4f %10.4f %10.1f %6d"
    # Example: "ISS (ZARYA)       123.4     45.6     1234.5    1.234 VIS   AOS 5m 30s    25544   45.1234  -76.5678     1234.5      0"

    line = line.strip()
    if not line or line.startswith('---') or line.startswith('NAME'):
        return None

    # Use fixed-width parsing based on format string
    try:
        # Split carefully - the name can have spaces
        parts = line.split()
        if len(parts) < 7:
            return None

        # Find the numeric values from the end
        next_event = ""
        visibility = ""

        # Work backwards to find visibility (VIS/DAY/ECL/HOR/---)
        vis_idx = -1
        for i, part in enumerate(parts):
            if part in ('VIS', 'DAY', 'ECL', 'HOR', '---'):
                vis_idx = i
                visibility = part
                break

        if vis_idx < 0:
            return None

        # Check if we have extended format (with NORAD, LAT, LON, APOGEE, FLARE after NEXT EVENT)
        # Extended format has at least 4 more numeric fields after the next_event
        norad_id = 0
        latitude = 0.0
        longitude = 0.0
        apogee = 0.0
        flare_status = 0

        # Find where extended data starts - it's the numeric fields at the end
        # Count backwards from end to find the extended fields
        extended_fields = []
        idx = len(parts) - 1
        while idx > vis_idx:
            try:
                float(parts[idx])
                extended_fields.insert(0, parts[idx])
                idx -= 1
            except ValueError:
                break

        # If we have at least 4 extended fields, parse them
        if len(extended_fields) >= 4:
            try:
                flare_status = int(extended_fields[-1])
                apogee = float(extended_fields[-2])
                longitude = float(extended_fields[-3])
                latitude = float(extended_fields[-4])
                if len(extended_fields) >= 5:
                    norad_id = int(extended_fields[-5])
                # Next event is between visibility and extended fields
                next_event_parts = parts[vis_idx + 1:idx + 1]
                next_event = ' '.join(next_event_parts)
            except (ValueError, IndexError):
                # Fall back to old parsing
                next_event = ' '.join(parts[vis_idx + 1:])
        else:
            # Old format - next event is everything after visibility
            next_event = ' '.join(parts[vis_idx + 1:])

        # Numeric values are before visibility
        try:
            range_rate = float(parts[vis_idx - 1])
            range_km = float(parts[vis_idx - 2])
            elevation = float(parts[vis_idx - 3])
            azimuth = float(parts[vis_idx - 4])
        except (ValueError, IndexError):
            return None

        # Name is everything before the numeric values
        name_parts = parts[:vis_idx - 4]
        name = ' '.join(name_parts)

        # Check for flare indicator in name
        has_flare = name.endswith(' F') or flare_status > 0
        if name.endswith(' F'):
            name = name[:-2].strip()

        return SatelliteData(
            name=name,
            azimuth=azimuth,
            elevation=elevation,
            range_km=range_km,
            range_rate=range_rate,
            visibility=visibility,
            next_event=next_event,
            norad_id=norad_id,
            latitude=latitude,
            longitude=longitude,
            apogee=apogee,
            flare_status=flare_status
        )
    except Exception:
        return None


def clear_screen():
    """Clear terminal screen."""
    print('\033[2J\033[H', end='')


def display_frame(frame: TrackingFrame, use_color: bool = True):
    """Display a parsed frame in a formatted way."""
    clear_screen()

    # Colors
    RESET = '\033[0m' if use_color else ''
    GREEN = '\033[32m' if use_color else ''
    YELLOW = '\033[33m' if use_color else ''
    CYAN = '\033[36m' if use_color else ''
    RED = '\033[31m' if use_color else ''
    BOLD = '\033[1m' if use_color else ''
    DIM = '\033[2m' if use_color else ''

    print(f"{BOLD}{CYAN}=== Visible Ephemeris Tracking Client ==={RESET}")
    print(f"{DIM}Timestamp: {frame.timestamp}{RESET}")
    print(f"Observer: {frame.observer_lat:.4f}, {frame.observer_lon:.4f}")
    print(f"Satellites: {len(frame.satellites)}")
    print()

    # Header
    header = f"{'NAME':<20} {'AZ':>8} {'EL':>8} {'RANGE':>10} {'RR':>8} {'VIS':>5} {'NEXT EVENT':<12} {'NORAD':>8} {'LAT':>10} {'LON':>10} {'APOGEE':>10} {'FLR':>4}"
    print(f"{BOLD}{header}{RESET}")
    print("-" * len(header))

    # Sort by elevation (highest first)
    sorted_sats = sorted(frame.satellites, key=lambda s: s.elevation, reverse=True)

    for sat in sorted_sats:
        # Choose color based on visibility
        if sat.visibility == 'VIS':
            color = GREEN
        elif sat.visibility == 'DAY':
            color = YELLOW
        elif sat.visibility == 'ECL':
            color = CYAN
        elif sat.visibility == 'HOR':
            color = DIM
        else:
            color = RESET

        # Flare indicator
        flare_mark = '*' if sat.flare_status > 0 else ''

        name_display = (sat.name[:18] + '..') if len(sat.name) > 18 else sat.name

        print(f"{color}{name_display:<20} {sat.azimuth:8.1f} {sat.elevation:8.1f} "
              f"{sat.range_km:10.1f} {sat.range_rate:8.3f} {sat.visibility:>5} "
              f"{sat.next_event:<12} {sat.norad_id:8d} {sat.latitude:10.4f} {sat.longitude:10.4f} "
              f"{sat.apogee:10.1f} {sat.flare_status:3d}{flare_mark}{RESET}")

    print()
    print(f"{DIM}Press Ctrl+C to exit{RESET}")


def output_json(frame: TrackingFrame):
    """Output frame as JSON."""
    data = {
        'timestamp': frame.timestamp,
        'observer': {
            'latitude': frame.observer_lat,
            'longitude': frame.observer_lon
        },
        'satellites': [
            {
                'name': sat.name,
                'azimuth': sat.azimuth,
                'elevation': sat.elevation,
                'range_km': sat.range_km,
                'range_rate': sat.range_rate,
                'visibility': sat.visibility,
                'next_event': sat.next_event,
                'norad_id': sat.norad_id,
                'latitude': sat.latitude,
                'longitude': sat.longitude,
                'apogee': sat.apogee,
                'flare_status': sat.flare_status
            }
            for sat in frame.satellites
        ]
    }
    print(json.dumps(data, indent=2))


class TrackingClient:
    """TCP client for the physics tracking stream."""

    FRAME_DELIMITER = b'\n---END_FRAME---\n'

    def __init__(self, host: str = 'localhost', port: int = 12346):
        self.host = host
        self.port = port
        self.socket = None
        self.buffer = b''

    def connect(self) -> bool:
        """Connect to the tracking server."""
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.connect((self.host, self.port))
            self.socket.settimeout(5.0)
            print(f"Connected to {self.host}:{self.port}")
            return True
        except socket.error as e:
            print(f"Connection failed: {e}", file=sys.stderr)
            return False

    def disconnect(self):
        """Disconnect from server."""
        if self.socket:
            try:
                self.socket.close()
            except socket.error:
                pass
            self.socket = None

    def receive_frame(self) -> Optional[str]:
        """Receive and return one complete frame."""
        while True:
            # Check if we have a complete frame in buffer
            if self.FRAME_DELIMITER in self.buffer:
                idx = self.buffer.index(self.FRAME_DELIMITER)
                frame_data = self.buffer[:idx]
                self.buffer = self.buffer[idx + len(self.FRAME_DELIMITER):]
                return frame_data.decode('utf-8', errors='replace')

            # Need more data
            try:
                chunk = self.socket.recv(4096)
                if not chunk:
                    return None  # Connection closed
                self.buffer += chunk
            except socket.timeout:
                continue
            except socket.error:
                return None

    def run(self, output_mode: str = 'display', use_color: bool = True):
        """Main client loop."""
        if not self.connect():
            return 1

        try:
            while True:
                raw_frame = self.receive_frame()
                if raw_frame is None:
                    print("Connection lost", file=sys.stderr)
                    break

                if output_mode == 'raw':
                    print(raw_frame)
                    print("---END_FRAME---")
                    continue

                frame = parse_frame(raw_frame)
                if frame is None:
                    continue

                if output_mode == 'json':
                    output_json(frame)
                else:
                    display_frame(frame, use_color)

        except KeyboardInterrupt:
            print("\nDisconnecting...")
        finally:
            self.disconnect()

        return 0


def main():
    parser = argparse.ArgumentParser(
        description='TCP Client for VisibleEphemeris Physics Stream',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                           Connect to localhost:12346
  %(prog)s --host 192.168.1.5        Connect to remote host
  %(prog)s --json                    Output as JSON (one object per frame)
  %(prog)s --raw                     Show raw frame data
  %(prog)s --no-color                Disable color output
        """
    )
    parser.add_argument('--host', default='localhost',
                        help='Server hostname (default: localhost)')
    parser.add_argument('--port', type=int, default=12346,
                        help='Server port (default: 12346)')
    parser.add_argument('--json', action='store_true',
                        help='Output frames as JSON')
    parser.add_argument('--raw', action='store_true',
                        help='Show raw frame data')
    parser.add_argument('--no-color', action='store_true',
                        help='Disable color output')

    args = parser.parse_args()

    if args.json:
        output_mode = 'json'
    elif args.raw:
        output_mode = 'raw'
    else:
        output_mode = 'display'

    client = TrackingClient(args.host, args.port)
    return client.run(output_mode, use_color=not args.no_color)


if __name__ == '__main__':
    sys.exit(main())
