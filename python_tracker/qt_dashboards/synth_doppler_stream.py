#!/usr/bin/env python3
"""Synthetic Doppler-shift TCP stream for end-to-end testing of live_od_panel.

Simulates a receiver that publishes one Doppler shift (Hz) per line to any
TCP client that connects. The shift is derived from a truth trajectory
integrated with the same two-body+J2 dynamics as live_od_filter, using a
given TLE for the initial state.

Usage:
    python3 -m qt_dashboards.synth_doppler_stream \\
        --tle-file tle_cache/amateur.txt --sat 'AO-91' \\
        --lat 39.1434 --lon -77.2014 --alt 0.153 \\
        --port 5555

Then in the Live OD panel, select "Raw Doppler-Hz socket stream",
host 127.0.0.1, port 5555, and Start.
"""
from __future__ import annotations

import argparse
import os
import socket
import sys
import threading
import time
from datetime import datetime, timedelta, timezone

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
_TRACKER_DIR = os.path.dirname(_HERE)
if _TRACKER_DIR not in sys.path:
    sys.path.insert(0, _TRACKER_DIR)
from live_od_filter import (  # noqa: E402
    LiveOdConfig, integrate_two_body_j2, predict_doppler,
    tle_epoch_state, elevation_deg, station_state_teme,
)


def load_tle(tle_file: str, sat_name: str):
    with open(tle_file, "r", errors="replace") as f:
        lines = [ln.rstrip("\n") for ln in f]
    i = 0
    while i + 2 < len(lines):
        if lines[i].strip().upper() == sat_name.strip().upper():
            return (lines[i].strip(), lines[i+1].strip(), lines[i+2].strip())
        i += 1
    raise SystemExit(f"Satellite '{sat_name}' not found in {tle_file}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tle-file", required=True)
    ap.add_argument("--sat", required=True, help="Satellite name (as it appears in the TLE)")
    ap.add_argument("--lat", type=float, required=True)
    ap.add_argument("--lon", type=float, required=True)
    ap.add_argument("--alt", type=float, default=0.0)
    ap.add_argument("--freq", type=float, default=2.4e9, help="Beacon transmit frequency (Hz)")
    ap.add_argument("--noise-hz", type=float, default=5.0)
    ap.add_argument("--period", type=float, default=1.0, help="Sample period (sec)")
    ap.add_argument("--min-elev", type=float, default=5.0)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--speedup", type=float, default=1.0,
                    help="Wall-clock accelerator (1 = real time, 10 = 10x fast forward)")
    args = ap.parse_args()

    name, l1, l2 = load_tle(args.tle_file, args.sat)
    print(f"[synth] TLE: {name}")
    t_epoch, jd0, r0, v0 = tle_epoch_state(name, l1, l2)
    print(f"[synth] Epoch: {t_epoch.isoformat()}   JD={jd0:.6f}")

    rng = np.random.default_rng(int(time.time()))

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    sock.listen(1)
    print(f"[synth] Listening on {args.host}:{args.port} for a client...")

    conn, addr = sock.accept()
    conn.settimeout(1.0)
    print(f"[synth] Client from {addr}. Streaming shifts every {args.period:.2f} s "
          f"(speedup {args.speedup}x).")
    t_ref = datetime.now(tz=timezone.utc)
    truth_state = np.concatenate([r0, v0])
    t_prev_sec = 0.0

    try:
        while True:
            t_sec = (datetime.now(tz=timezone.utc) - t_ref).total_seconds() * args.speedup
            if t_sec > t_prev_sec:
                truth_state = integrate_two_body_j2(truth_state, 0.0, t_sec - t_prev_sec)
                t_prev_sec = t_sec
            jd = jd0 + t_sec / 86400.0
            r_stn, _ = station_state_teme(args.lat, args.lon, args.alt, jd)
            el = elevation_deg(truth_state[0:3], r_stn)
            if el >= args.min_elev:
                x = np.concatenate([truth_state, [0.0, 0.0]])
                f_true, _, _ = predict_doppler(
                    x, jd, t_sec, (args.lat, args.lon, args.alt), args.freq)
                shift = (f_true - args.freq) + rng.normal(0.0, args.noise_hz)
                try:
                    conn.sendall(f"{shift:.3f}\n".encode())
                except OSError:
                    print("[synth] Client disconnected.")
                    break
            time.sleep(args.period)
    except KeyboardInterrupt:
        print("\n[synth] Interrupted.")
    finally:
        conn.close()
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
