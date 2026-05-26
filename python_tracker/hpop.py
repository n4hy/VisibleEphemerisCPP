"""High-Precision Orbit Propagator backend for python_tracker.

Wraps the compiled `ve_hpop` pybind11 module (the same C++ force-model
integrator used by the C++ tracker) and reproduces the C++ observer/visibility
geometry (see src/observer.cpp and src/visibility.cpp) so that the Python HPOP
path matches the C++ HPOP path. The integration frame is TEME-as-pseudo-inertial,
identical to SGP4's output frame.
"""
import os
import sys
import math
import datetime
import numpy as np

# ---- locate and import the compiled ve_hpop module -------------------------
ve_hpop = None
_import_error = None
try:
    import ve_hpop  # already on PYTHONPATH
except ImportError as e:
    _import_error = e
    # Fall back to the in-tree CMake build directory: ../build
    _candidates = [
        os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build")),
        os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "Release")),
    ]
    for _d in _candidates:
        if os.path.isdir(_d) and _d not in sys.path:
            sys.path.insert(0, _d)
    try:
        import ve_hpop  # noqa: F811
        _import_error = None
    except ImportError as e2:
        _import_error = e2


def available():
    return ve_hpop is not None


def import_hint():
    return ("ve_hpop module not found. Build it with:\n"
            "  cd build && cmake .. -DBUILD_PYTHON_BINDINGS=ON "
            "-DPython_EXECUTABLE=$(which python3) && cmake --build . --target ve_hpop\n"
            f"(original import error: {_import_error})")


# ---- constants (match include/types.hpp) -----------------------------------
DEG2RAD = math.pi / 180.0
RAD2DEG = 180.0 / math.pi
EARTH_RADIUS_KM = 6378.137
OMEGA = 7.2921159e-5  # Earth rotation rate (rad/s)


def jd_from_datetime(dt):
    """UTC datetime -> Julian Date (matches NumericalPropagator::julianFromTimePoint)."""
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=datetime.timezone.utc)
    return 2440587.5 + dt.timestamp() / 86400.0


def _observer_eci(lat_deg, lon_deg, alt_km, gst):
    """WGS84 geodetic observer -> ECI (mirrors Observer::getPositionECI)."""
    lat = lat_deg * DEG2RAD
    lon = lon_deg * DEG2RAD
    a = 6378.137
    f = 1.0 / 298.257223563
    e2 = 2 * f - f * f
    N = a / math.sqrt(1 - e2 * math.sin(lat) ** 2)
    xe = (N + alt_km) * math.cos(lat) * math.cos(lon)
    ye = (N + alt_km) * math.cos(lat) * math.sin(lon)
    ze = (N * (1 - e2) + alt_km) * math.sin(lat)
    ct, st = math.cos(gst), math.sin(gst)
    return np.array([xe * ct - ye * st, xe * st + ye * ct, ze])


def _look_angle(sat_eci, obs_eci, lat_deg, lon_deg, gst):
    """ECI -> topocentric SEZ az/el/range (mirrors Observer::calculateLookAngle)."""
    r = sat_eci - obs_eci
    lat = lat_deg * DEG2RAD
    lst = gst + lon_deg * DEG2RAD
    sL, cL = math.sin(lat), math.cos(lat)
    sLS, cLS = math.sin(lst), math.cos(lst)
    s = sL * cLS * r[0] + sL * sLS * r[1] - cL * r[2]
    e = -sLS * r[0] + cLS * r[1]
    z = cL * cLS * r[0] + cL * sLS * r[1] + sL * r[2]
    rng = math.sqrt(s * s + e * e + z * z)
    az = math.atan2(e, -s)
    if az < 0:
        az += 2 * math.pi
    sin_el = (z / rng) if rng > 1e-9 else 0.0
    sin_el = max(-1.0, min(1.0, sin_el))
    return az * RAD2DEG, math.asin(sin_el) * RAD2DEG, rng


def _sun_eci_meeus(jd):
    """Geocentric Sun ECI (km), mirrors VisibilityCalculator::getSunPositionECI."""
    n = jd - 2451545.0
    L = (280.460 + 0.9856474 * n) % 360.0
    if L < 0:
        L += 360.0
    g = (357.528 + 0.9856003 * n) % 360.0
    if g < 0:
        g += 360.0
    lam = (L + 1.915 * math.sin(g * DEG2RAD) + 0.020 * math.sin(2 * g * DEG2RAD)) * DEG2RAD
    eps = (23.439 - 0.0000004 * n) * DEG2RAD
    R = 149597870.7
    return np.array([R * math.cos(lam),
                     R * math.cos(eps) * math.sin(lam),
                     R * math.sin(eps) * math.sin(lam)])


def _unit(v):
    n = np.linalg.norm(v)
    return v / n if n > 0 else v


def _visibility(sat_eci, obs_eci, sun_eci, el_deg):
    """Mirrors VisibilityCalculator::calculateState; returns 'YES' | 'DAY' | 'NO'."""
    sat_mag = np.linalg.norm(sat_eci)
    umbra = math.asin(EARTH_RADIUS_KM / sat_mag)
    angle = math.acos(max(-1.0, min(1.0, float(np.dot(_unit(sat_eci), _unit(sun_eci))))))
    lit = (angle < math.pi / 2.0) or ((math.pi - angle) >= umbra)
    if not lit:
        return "NO"
    sun_el = (math.pi / 2.0) - math.acos(
        max(-1.0, min(1.0, float(np.dot(_unit(obs_eci), _unit(sun_eci))))))
    if el_deg > 0.0 and sun_el <= (-12.0 * DEG2RAD):
        return "YES"
    return "DAY"


def make_propagator(tle, degree=20, drag=True, srp=True, thirdbody=True):
    """Construct a ve_hpop.Propagator from a TLE dict {name,line1,line2}."""
    if ve_hpop is None:
        raise ImportError(import_hint())
    return ve_hpop.Propagator(tle["name"], tle["line1"], tle["line2"],
                              degree=degree, drag=drag, srp=srp, thirdbody=thirdbody)


def compute_state(prop, lat_deg, lon_deg, alt_km, dt_utc):
    """Full instantaneous state for the given observer and UTC datetime.

    Returns a dict: az, el, range_km, range_rate, lat, lon, alt_km, visibility,
    sun_eci, sat_eci, obs_eci.
    """
    jd = jd_from_datetime(dt_utc)
    r, v = prop.propagate_jd(jd)
    sat_r = np.array(r)
    sat_v = np.array(v)
    gst = ve_hpop.gmst_rad(jd)

    obs_r = _observer_eci(lat_deg, lon_deg, alt_km, gst)
    obs_v = np.array([-OMEGA * obs_r[1], OMEGA * obs_r[0], 0.0])

    az, el, rng = _look_angle(sat_r, obs_r, lat_deg, lon_deg, gst)

    rel_r = sat_r - obs_r
    rel_v = sat_v - obs_v
    rel_mag = np.linalg.norm(rel_r)
    range_rate = float(np.dot(rel_r, rel_v) / rel_mag) if rel_mag > 1e-9 else 0.0

    glat, glon, galt = prop.geodetic_jd(jd)

    sun_eci = _sun_eci_meeus(jd)
    vis = _visibility(sat_r, obs_r, sun_eci, el)

    return {
        "az": az, "el": el, "range_km": rng, "range_rate": range_rate,
        "lat": glat, "lon": glon, "alt_km": galt, "visibility": vis,
        "sun_eci": sun_eci, "sat_eci": sat_r, "obs_eci": obs_r,
    }


def ground_track(prop, dt_center_utc, minutes):
    """List of [lat, lon] sampled +/- `minutes` around dt_center_utc at 1-min steps."""
    pts = []
    for i in range(-minutes, minutes + 1):
        dt = dt_center_utc + datetime.timedelta(minutes=i)
        glat, glon, _ = prop.geodetic_jd(jd_from_datetime(dt))
        pts.append([glat, glon])
    return pts
