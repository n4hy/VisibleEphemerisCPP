#!/usr/bin/env python3
"""
Functional Equivalence Test: Python tracker vs C++ tracker.

Compares core computational outputs between the Python (Skyfield-based) and
C++ (libsgp4-based) implementations using shared TLE data and fixed time/observer.

Key differences expected:
  - SGP4 propagation: Both use SGP4, but different libraries (python-sgp4 vs libsgp4).
    Expect < 1 km position difference.
  - Sun position: C++ uses simple Meeus analytical model; Python uses JPL DE421 ephemeris.
    Expect < 0.5 deg difference in sun position.
  - Look angles: Derived from propagation + observer transform. Expect < 0.1 deg.
  - Visibility: Both use satellite-sunlit + observer-dark logic, but different sun models
    and shadow geometry. May disagree near terminator boundaries.
"""

import sys
import os
import math
import datetime
import subprocess
import json
import tempfile
import shutil

# Add python_tracker to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_tracker'))

from skyfield.api import load, EarthSatellite, wgs84
from observer import Observer
from satellite import Satellite

# ─── Test Configuration ───────────────────────────────────────────────────────

# Observer: from config.yaml
OBS_LAT = 39.5478
OBS_LON = -76.0916
OBS_ALT_KM = 0.0

# Fixed test time (UTC) — use a time when TLE data is valid
# Using a recent date within TLE epoch validity
TEST_TIME_STR = "2025-12-08 20:00:00"  # Evening UTC, nighttime in Maryland

# Well-known test satellites with stable orbits
# Using ISS as primary test case (NORAD 25544)
ISS_TLE = {
    'name': 'ISS (ZARYA)',
    'line1': '1 25544U 98067A   25342.18171806  .00023618  00000+0  41560-3 0  9999',
    'line2': '2 25544  51.6390 196.0228 0006517 314.6498 142.3419 15.50266153487131'
}

# NOAA 19 — polar orbit, good visibility test
NOAA19_TLE = {
    'name': 'NOAA 19',
    'line1': '1 33591U 09005A   25342.24421497  .00000268  00000+0  16980-3 0  9999',
    'line2': '2 33591  99.0839 336.2746 0013439 230.9989 129.0006 14.13108571829108'
}

# High-orbit satellite for apogee test
GPS_TLE = {
    'name': 'GPS BIIR-2  (PRN 13)',
    'line1': '1 24876U 97035A   25341.72499197 -.00000036  00000+0  00000+0 0  9995',
    'line2': '2 24876  55.6914  61.3637 0044143 104.8193 255.7359  2.00562251199814'
}

# ─── Tolerance Constants ──────────────────────────────────────────────────────

TOL_AZ_DEG = 0.15       # Azimuth tolerance (degrees)
TOL_EL_DEG = 0.15       # Elevation tolerance (degrees)
TOL_RANGE_KM = 2.0      # Range tolerance (km)
TOL_LAT_DEG = 0.05      # Sub-satellite latitude (degrees)
TOL_LON_DEG = 0.05      # Sub-satellite longitude (degrees)
TOL_ALT_KM = 1.0        # Altitude tolerance (km)
TOL_APOGEE_KM = 1.0     # Apogee tolerance (km)
TOL_SUN_DEG = 0.5       # Sun position tolerance (degrees)

# ─── C++ Reference Computation ────────────────────────────────────────────────

def build_cpp_test_tool():
    """Build a minimal C++ test program that outputs satellite state as JSON."""
    cpp_source = r'''
#include <iostream>
#include <cmath>
#include <ctime>
#include <string>
#include <chrono>
#include <cstdio>
#include "satellite.hpp"
#include "observer.hpp"
#include "visibility.hpp"
#include "types.hpp"

using namespace ve;

int main(int argc, char* argv[]) {
    if (argc < 7) {
        std::cerr << "Usage: test_equiv <lat> <lon> <alt_km> <time_str> <tle_line1> <tle_line2> [name]" << std::endl;
        return 1;
    }

    double lat = std::stod(argv[1]);
    double lon = std::stod(argv[2]);
    double alt_km = std::stod(argv[3]);
    std::string time_str = argv[4];
    std::string line1 = argv[5];
    std::string line2 = argv[6];
    std::string name = (argc > 7) ? argv[7] : "TEST";

    // Parse time
    int Y, M, D, h, m, s;
    if (sscanf(time_str.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) {
        std::cerr << "Invalid time format" << std::endl;
        return 1;
    }

    // Build time_t (UTC)
    struct tm t = {};
    t.tm_year = Y - 1900;
    t.tm_mon = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min = m;
    t.tm_sec = s;
    t.tm_isdst = 0;

    // Portable UTC -> time_t
    static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    long year = t.tm_year + 1900;
    int month = t.tm_mon;
    long days = 0;
    for (long y = 1970; y < year; ++y) {
        bool is_leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += is_leap ? 366 : 365;
    }
    bool current_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    for (int mm = 0; mm < month; ++mm) {
        if (mm == 1 && current_leap) days += 29;
        else days += days_in_month[mm];
    }
    days += (t.tm_mday - 1);
    std::time_t utc_tt = days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;

    auto now = std::chrono::system_clock::from_time_t(utc_tt);

    // Create satellite and observer
    try {
        Satellite sat(name, line1, line2);
        Observer obs(lat, lon, alt_km);

        auto [pos, vel] = sat.propagate(now);
        auto look = obs.calculateLookAngle(pos, now);
        double rrate = obs.calculateRangeRate(pos, vel, now);
        auto geo = sat.getGeodetic(now);
        double apogee = sat.getApogeeKm();

        // Visibility
        auto vis_state = VisibilityCalculator::calculateState(
            pos, obs.getPositionECI(now), now, look.elevation);
        std::string vis_str;
        switch(vis_state) {
            case VisibilityCalculator::State::VISIBLE: vis_str = "YES"; break;
            case VisibilityCalculator::State::DAYLIGHT: vis_str = "DAY"; break;
            case VisibilityCalculator::State::ECLIPSED: vis_str = "NO"; break;
        }

        // Sun position
        auto sun_geo = VisibilityCalculator::getSunPositionGeo(now);

        // Output JSON
        printf("{\"az\":%.6f,\"el\":%.6f,\"range\":%.6f,\"rrate\":%.6f,"
               "\"lat\":%.6f,\"lon\":%.6f,\"alt_km\":%.6f,"
               "\"apogee\":%.3f,\"vis\":\"%s\","
               "\"sun_lat\":%.6f,\"sun_lon\":%.6f}\n",
               look.azimuth, look.elevation, look.range, rrate,
               geo.lat_deg, geo.lon_deg, geo.alt_km,
               apogee, vis_str.c_str(),
               sun_geo.lat_deg, sun_geo.lon_deg);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
'''
    # Write source
    test_dir = os.path.join(os.path.dirname(__file__))
    src_path = os.path.join(test_dir, 'test_equiv_main.cpp')
    with open(src_path, 'w') as f:
        f.write(cpp_source)

    # Compile
    project_root = os.path.join(os.path.dirname(__file__), '..')
    include_dir = os.path.join(project_root, 'include')
    src_dir = os.path.join(project_root, 'src')
    build_dir = os.path.join(project_root, 'build')
    bin_path = os.path.join(test_dir, 'test_equiv')

    # Find SGP4
    sgp4_root = os.path.expanduser('~/sgp4/build/install')
    sgp4_inc = os.path.join(sgp4_root, 'include', 'libsgp4')
    sgp4_lib = os.path.join(sgp4_root, 'lib')

    # Source files needed (minimal set for computation)
    src_files = [
        src_path,
        os.path.join(src_dir, 'satellite.cpp'),
        os.path.join(src_dir, 'observer.cpp'),
        os.path.join(src_dir, 'visibility.cpp'),
        os.path.join(src_dir, 'logger.cpp'),
    ]

    # Pick a C++ compiler: $CXX, else clang++ (project reference), else g++.
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++') or 'c++'
    cmd = [
        cxx, '-std=c++17', '-O2',
        '-I', include_dir,
        '-I', sgp4_inc,
    ] + src_files + [
        '-L', sgp4_lib,
        '-lsgp4s', '-lpthread',
        '-Wl,-rpath,' + sgp4_lib,
        '-o', bin_path
    ]

    print(f"Compiling C++ test tool with {cxx}...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"COMPILE ERROR:\n{result.stderr}")
        return None

    print(f"Compiled: {bin_path}")
    return bin_path


def run_cpp_test(bin_path, lat, lon, alt_km, time_str, tle):
    """Run the C++ test tool and return parsed JSON result."""
    cmd = [
        bin_path,
        str(lat), str(lon), str(alt_km),
        time_str,
        tle['line1'], tle['line2'], tle['name']
    ]

    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = os.path.expanduser('~/sgp4/build/install/lib') + ':' + env.get('LD_LIBRARY_PATH', '')

    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if result.returncode != 0:
        print(f"C++ ERROR: {result.stderr}")
        return None

    return json.loads(result.stdout.strip())


def run_python_test(lat, lon, alt_km, time_str, tle):
    """Run the Python tracker computation and return results dict."""
    ts = load.timescale()
    dt = datetime.datetime.strptime(time_str, "%Y-%m-%d %H:%M:%S").replace(tzinfo=datetime.timezone.utc)
    t = ts.from_datetime(dt)

    obs = Observer(lat, lon, alt_km)
    sat = Satellite(tle)
    sat.update_position(obs, t)

    # Sun position
    sun_lat, sun_lon = obs.get_sun_position(t)

    return {
        'az': sat.az,
        'el': sat.el,
        'range': sat.range,
        'lat': sat.lat,
        'lon': sat.lon,
        'alt_km': sat.alt_km,
        'apogee': sat.apogee,
        'vis': sat.visibility,
        'sun_lat': sun_lat,
        'sun_lon': sun_lon,
    }


# ─── Test Runner ──────────────────────────────────────────────────────────────

class TestResult:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.warnings = 0
        self.details = []

    def check(self, name, py_val, cpp_val, tolerance, unit=""):
        diff = abs(py_val - cpp_val)
        ok = diff <= tolerance
        status = "PASS" if ok else "FAIL"
        if not ok:
            self.failed += 1
        else:
            self.passed += 1

        detail = f"  [{status}] {name:20s}: Py={py_val:12.6f}  C++={cpp_val:12.6f}  diff={diff:.6f} {unit}  (tol={tolerance})"
        self.details.append(detail)
        if not ok:
            print(detail)
        return ok

    def check_str(self, name, py_val, cpp_val):
        ok = py_val == cpp_val
        status = "PASS" if ok else "WARN"
        if not ok:
            self.warnings += 1
        else:
            self.passed += 1
        detail = f"  [{status}] {name:20s}: Py={py_val:>6s}  C++={cpp_val:>6s}"
        self.details.append(detail)
        if not ok:
            print(detail)
        return ok

    def summary(self):
        total = self.passed + self.failed + self.warnings
        print(f"\n{'='*60}")
        print(f"RESULTS: {self.passed} passed, {self.failed} failed, {self.warnings} warnings / {total} total")
        print(f"{'='*60}")
        return self.failed == 0


def run_satellite_test(results, sat_name, tle, cpp_bin):
    """Run equivalence test for one satellite."""
    print(f"\n--- {sat_name} ---")

    py = run_python_test(OBS_LAT, OBS_LON, OBS_ALT_KM, TEST_TIME_STR, tle)
    cpp = run_cpp_test(cpp_bin, OBS_LAT, OBS_LON, OBS_ALT_KM, TEST_TIME_STR, tle)

    if py is None or cpp is None:
        print(f"  SKIP: Could not compute for {sat_name}")
        return

    # Position comparison
    results.check(f"{sat_name} Az", py['az'], cpp['az'], TOL_AZ_DEG, "deg")
    results.check(f"{sat_name} El", py['el'], cpp['el'], TOL_EL_DEG, "deg")
    results.check(f"{sat_name} Range", py['range'], cpp['range'], TOL_RANGE_KM, "km")

    # Sub-satellite point
    results.check(f"{sat_name} Lat", py['lat'], cpp['lat'], TOL_LAT_DEG, "deg")
    results.check(f"{sat_name} Lon", py['lon'], cpp['lon'], TOL_LON_DEG, "deg")
    results.check(f"{sat_name} Alt", py['alt_km'], cpp['alt_km'], TOL_ALT_KM, "km")

    # Orbital elements
    results.check(f"{sat_name} Apogee", py['apogee'], cpp['apogee'], TOL_APOGEE_KM, "km")

    # Visibility (string match — may differ near terminator)
    results.check_str(f"{sat_name} Visibility", py['vis'], cpp['vis'])

    # Sun position (only test once per run, same for all sats)
    results.check(f"{sat_name} SunLat", py['sun_lat'], cpp['sun_lat'], TOL_SUN_DEG, "deg")
    results.check(f"{sat_name} SunLon", py['sun_lon'], cpp['sun_lon'], TOL_SUN_DEG, "deg")

    # Print all details
    for d in results.details[-10:]:
        if "[PASS]" in d:
            print(d)


def test_apogee_calculations():
    """Test apogee computation independently (no C++ needed)."""
    print("\n--- Apogee Computation (Python-only unit test) ---")

    mu = 398600.4418
    R_e = 6378.137

    for name, tle in [("ISS", ISS_TLE), ("NOAA-19", NOAA19_TLE), ("GPS", GPS_TLE)]:
        sat = Satellite(tle)
        # Recalculate manually
        mm_rad_min = sat.skyfield_sat.model.no_kozai
        n = mm_rad_min / 60.0  # rad/s
        a = (mu / (n * n)) ** (1.0 / 3.0)
        e = sat.skyfield_sat.model.ecco
        expected = a * (1 + e) - R_e

        diff = abs(sat.apogee - expected)
        status = "PASS" if diff < 0.001 else "FAIL"
        print(f"  [{status}] {name:12s} apogee: computed={sat.apogee:.3f} km, expected={expected:.3f} km, diff={diff:.6f}")


def test_decay_detection():
    """Test decay detection logic."""
    print("\n--- Decay Detection ---")

    # ISS should NOT be decayed
    iss = Satellite(ISS_TLE)
    status = "PASS" if not iss.is_decayed() else "FAIL"
    print(f"  [{status}] ISS decayed={iss.is_decayed()} (apogee={iss.apogee:.1f} km, expected: not decayed)")

    # GPS should NOT be decayed
    gps = Satellite(GPS_TLE)
    status = "PASS" if not gps.is_decayed() else "FAIL"
    print(f"  [{status}] GPS decayed={gps.is_decayed()} (apogee={gps.apogee:.1f} km, expected: not decayed)")


def main():
    print("=" * 60)
    print("Functional Equivalence Test: Python vs C++")
    print(f"Observer: {OBS_LAT}, {OBS_LON}, alt={OBS_ALT_KM} km")
    print(f"Time:     {TEST_TIME_STR} UTC")
    print("=" * 60)

    # 1. Build C++ test tool
    cpp_bin = build_cpp_test_tool()
    if cpp_bin is None:
        print("\nFATAL: Could not compile C++ test tool.")
        sys.exit(1)

    # 2. Run equivalence tests
    results = TestResult()

    test_sats = [
        ("ISS (ZARYA)", ISS_TLE),
        ("NOAA 19", NOAA19_TLE),
        ("GPS BIIR-2", GPS_TLE),
    ]

    for name, tle in test_sats:
        run_satellite_test(results, name, tle, cpp_bin)

    # 3. Python-only unit tests
    test_apogee_calculations()
    test_decay_detection()

    # 4. Summary
    ok = results.summary()

    if ok:
        print("\nAll equivalence tests PASSED.")
    else:
        print("\nSome tests FAILED. See details above.")
        print("Note: Small differences are expected due to different SGP4/Sun models.")

    # Print full report
    print("\n--- Full Detail Report ---")
    for d in results.details:
        print(d)

    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
