"""Per-satellite orbital model for the Python tracker.

Wraps a Skyfield EarthSatellite (SGP4) and, optionally, the compiled ve_hpop
High-Precision Orbit Propagator via the `hpop` backend. Computes the look angle,
range rate, sub-satellite point, optical visibility, Iridium flare status,
ground track, and pass predictions. The HPOP path reproduces the C++ observer
and visibility geometry so the two high-precision implementations agree.
Functional twin of the C++ Satellite (src/satellite.cpp).
"""
from skyfield.api import load, EarthSatellite, wgs84
import datetime
import math
import numpy as np
import hpop  # High-Precision Orbit Propagator backend (wraps ve_hpop)

# Singleton timescale instance to avoid repeated loads
_timescale = None

def _get_timescale():
    global _timescale
    if _timescale is None:
        _timescale = load.timescale()
    return _timescale

class Satellite:
    def __init__(self, tle, hpop_opts=None):
        self.name = tle['name']
        self.line1 = tle['line1']
        self.line2 = tle['line2']

        ts = _get_timescale()
        self.skyfield_sat = EarthSatellite(self.line1, self.line2, self.name, ts)
        self.norad_id = self.skyfield_sat.model.satnum

        # Optional High-Precision Orbit Propagator backend. When set, the
        # numerical force-model integrator replaces SGP4/Skyfield for the
        # satellite state. Synthetic Sun/Moon objects (norad <= 0) are skipped.
        self.hpop = None
        if hpop_opts and self.norad_id and self.norad_id > 0:
            try:
                self.hpop = hpop.make_propagator(tle, **hpop_opts)
                if not self.hpop.valid:
                    self.hpop = None
            except Exception as e:
                print(f"[HPOP] init failed for {self.name}: {e}; using Skyfield")
                self.hpop = None

        # State Cache
        self.az = 0.0
        self.el = 0.0
        self.range = 0.0
        self.range_rate = 0.0  # km/s - positive = moving away
        self.lat = 0.0
        self.lon = 0.0
        self.alt_km = 0.0
        self.visibility = "NO" # YES, DAY, NO
        self.trail = [] # List of [lat, lon]
        self.next_event = "N/A"
        self.apogee = self._calculate_apogee() # Calculated from TLE orbital elements
        self.flare_status = 0  # 0=None, 1=Near (0.5-1.0 deg), 2=Hit (<0.5 deg)

        # Pass Prediction Cache
        self.passes = [] # List of (time, event_type_int)
        self.last_pass_calc = None
        self.is_computing = False

    def _calculate_apogee(self):
        """Calculate apogee (km above Earth surface) from TLE orbital elements."""
        try:
            # Get mean motion (revolutions per day) from TLE
            mm = self.skyfield_sat.model.no_kozai  # radians per minute
            n = mm / 60.0  # Convert to radians per second

            # Earth's gravitational parameter (km^3/s^2)
            mu = 398600.4418
            EARTH_RADIUS_KM = 6378.137

            # Calculate semi-major axis using Kepler's third law
            # n = sqrt(mu / a^3)  =>  a = (mu / n^2)^(1/3)
            a = (mu / (n * n)) ** (1.0 / 3.0)

            # Get eccentricity from TLE
            e = self.skyfield_sat.model.ecco

            # Apogee = a * (1 + e) - Earth_radius
            return a * (1 + e) - EARTH_RADIUS_KM
        except Exception:
            return 0.0

    def is_decayed(self):
        """Check if satellite appears to have decayed (apogee < 80km)."""
        return self.apogee < 80.0

    def _to_utc_datetime(self, t_now):
        if isinstance(t_now, datetime.datetime):
            dt = t_now
            if dt.tzinfo is None:
                dt = dt.replace(tzinfo=datetime.timezone.utc)
            return dt
        # Skyfield Time
        return t_now.utc_datetime()

    def update_position_hpop(self, observer, t_now, trail_mins=0):
        """HPOP backend update mirroring the C++ high-precision path."""
        dt = self._to_utc_datetime(t_now)
        st = hpop.compute_state(self.hpop, observer.lat_deg, observer.lon_deg,
                                observer.alt_km, dt)
        self.az = st["az"]
        self.el = st["el"]
        self.range = st["range_km"]
        self.range_rate = st["range_rate"]
        self.lat = st["lat"]
        self.lon = st["lon"]
        self.alt_km = st["alt_km"]
        self.visibility = st["visibility"]

        # Flare status (parity with Skyfield path: simple sun-sat-observer angle for Iridium).
        self.flare_status = 0
        if self.visibility == "YES" and "IRIDIUM" in self.name.upper():
            sat_to_sun = st["sun_eci"] - st["sat_eci"]
            sat_to_obs = st["obs_eci"] - st["sat_eci"]
            n1 = np.linalg.norm(sat_to_sun)
            n2 = np.linalg.norm(sat_to_obs)
            if n1 > 0 and n2 > 0:
                cos_a = np.dot(sat_to_sun / n1, sat_to_obs / n2)
                angle_deg = math.degrees(math.acos(max(-1.0, min(1.0, cos_a))))
                if angle_deg < 0.5:
                    self.flare_status = 2
                elif angle_deg < 1.0:
                    self.flare_status = 1

        if trail_mins > 0:
            self.trail = hpop.ground_track(self.hpop, dt, trail_mins)

    def update_position(self, observer, t_now, trail_mins=0):
        if self.hpop is not None:
            self.update_position_hpop(observer, t_now, trail_mins)
            return

        # Ensure t_now is a Skyfield Time object
        ts = _get_timescale()
        if isinstance(t_now, datetime.datetime):
            t = ts.from_datetime(t_now)
        else:
            t = t_now

        # 1. Look Angle (Az/El/Range) relative to observer
        difference = self.skyfield_sat - observer.location
        topocentric = difference.at(t)
        el, az, r = topocentric.altaz()

        self.az = az.degrees
        self.el = el.degrees
        self.range = r.km

        # 2. Calculate Range Rate (velocity component along line of sight)
        # Get satellite position and velocity in GCRS
        geocentric = self.skyfield_sat.at(t)
        sat_pos = geocentric.position.km
        sat_vel = geocentric.velocity.km_per_s

        # Get observer position in GCRS
        obs_geocentric = observer.location.at(t)
        obs_pos = obs_geocentric.position.km

        # Range vector (satellite - observer)
        range_vec = sat_pos - obs_pos
        range_mag = np.linalg.norm(range_vec)

        if range_mag > 0:
            # Range rate is projection of velocity onto unit range vector
            range_unit = range_vec / range_mag
            self.range_rate = np.dot(sat_vel, range_unit)
        else:
            self.range_rate = 0.0

        # 3. Geodetic Position (Lat/Lon/Alt)
        subpoint = wgs84.subpoint(geocentric)
        self.lat = subpoint.latitude.degrees
        self.lon = subpoint.longitude.degrees
        self.alt_km = subpoint.elevation.km

        # 4. Visibility Logic (must match C++ VisibilityCalculator::calculateState)
        # YES (VISIBLE) = satellite above the horizon (el > 0)
        #                 AND observer in astronomical twilight or darker (sun <= -12 deg)
        #                 AND satellite sunlit (not in Earth's shadow)
        # DAY (DAYLIGHT) = sunlit satellite that fails one of the above (daylight/twilight
        #                  or below the horizon) — present but not visually observable
        # NO  (ECLIPSED) = satellite in Earth's shadow
        sat_sunlit = geocentric.is_sunlit(observer.eph)  # correct use of is_sunlit (satellite)
        sun_alt = observer.sun_altitude_deg(t)

        if not sat_sunlit:
            self.visibility = "NO"
        elif self.el > 0.0 and sun_alt <= -12.0:
            self.visibility = "YES" # Visual pass!
        else:
            self.visibility = "DAY" # Sunlit but not naked-eye visible

        # 5. Flare Status (simplified - check for Iridium-like sun glints)
        self.flare_status = 0
        if self.visibility == "YES" and "IRIDIUM" in self.name.upper():
            # Calculate sun-satellite-observer angle for potential flare
            sun_pos = observer.get_sun_position_eci(t)
            if sun_pos is not None:
                # Vector from satellite to sun
                sat_to_sun = sun_pos - sat_pos
                sat_to_sun_unit = sat_to_sun / np.linalg.norm(sat_to_sun)

                # Vector from satellite to observer
                sat_to_obs = obs_pos - sat_pos
                sat_to_obs_unit = sat_to_obs / np.linalg.norm(sat_to_obs)

                # Reflection angle (simplified - assumes flat reflector)
                # Ideal reflection: angle(sun-sat-obs) near 0 for specular reflection
                cos_angle = np.dot(sat_to_sun_unit, sat_to_obs_unit)
                angle_deg = np.degrees(np.arccos(np.clip(cos_angle, -1, 1)))

                # Flare detection thresholds
                if angle_deg < 0.5:
                    self.flare_status = 2  # Hit
                elif angle_deg < 1.0:
                    self.flare_status = 1  # Near

        # 6. Trail Calculation (Ground Track)
        if trail_mins > 0:
            self.trail = self.calculate_ground_track(t, trail_mins)

        # 7. Apogee is computed once from TLE in __init__, no need to update here

    def calculate_ground_track(self, t_center, minutes):
        # Generate points +/- minutes
        ts = _get_timescale()
        points = []

        # Optimize: 1 minute steps
        start_dt = t_center.utc_datetime() - datetime.timedelta(minutes=minutes)
        # Create a vector of times for efficiency?
        # Skyfield handles arrays of times efficiently
        times = ts.from_datetimes([start_dt + datetime.timedelta(minutes=i) for i in range(minutes * 2 + 1)])

        g = self.skyfield_sat.at(times)
        sub = wgs84.subpoint(g)

        lats = sub.latitude.degrees
        lons = sub.longitude.degrees

        for lat, lon in zip(lats, lons):
            points.append([lat, lon])

        return points

    def compute_passes(self, observer_location, t_start_ts, duration_days=1, min_el=0.0):
        """
        Computes pass events (rise/culminate/set) for the given duration.
        NOTE: This is computationally expensive and should be run in a background thread.
        """
        ts = _get_timescale()
        t0 = t_start_ts
        t1 = ts.from_datetime(t_start_ts.utc_datetime() + datetime.timedelta(days=duration_days))

        t, events = self.skyfield_sat.find_events(observer_location, t0, t1, altitude_degrees=min_el)

        # Store as list of (time_obj, event_code)
        # event_code: 0=rise, 1=culminate, 2=set
        self.passes = list(zip(t, events))
        self.last_pass_calc = t_start_ts
        return self.passes

    def get_next_event_text(self, t_now_ts):
        """
        Returns a string describing the next event based on cached passes.
        e.g., "AOS 10m 30s" or "LOS 2m 10s"
        """
        if not self.passes:
            return "Calculating..." if self.is_computing else "N/A"

        # Find first event in the future
        next_evt = None
        for t, code in self.passes:
            if t.tt > t_now_ts.tt: # Simple time comparison
                next_evt = (t, code)
                break

        if not next_evt:
            return "None < 24h"

        t_event, code = next_evt

        # Format difference
        diff_seconds = (t_event.utc_datetime() - t_now_ts.utc_datetime()).total_seconds()

        # If diff is negative (shouldn't be due to loop check, but safety), return N/A
        if diff_seconds < 0: return "N/A"

        m, s = divmod(int(diff_seconds), 60)
        h, m = divmod(m, 60)

        time_str = f"{m}m {s}s"
        if h > 0: time_str = f"{h}h {time_str}"

        # Code: 0=Rise (AOS), 1=Culminate, 2=Set (LOS)
        # Note: If next is Rise, we are currently AOS? No, Rise means AOS is coming.
        # If next is Set, we are likely currently UP (AOS happened).
        # C++ Logic: "next.is_aos ? 'AOS ' : 'LOS '"
        # Skyfield: 0=Rise, 1=Culminate, 2=Set

        label = ""
        if code == 0: label = "AOS"
        elif code == 1: label = "CUL"
        elif code == 2: label = "LOS"

        return f"{label} {time_str}"
