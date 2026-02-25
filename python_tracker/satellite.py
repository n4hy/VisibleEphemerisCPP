from skyfield.api import load, EarthSatellite, wgs84
import datetime
import math

# Singleton timescale instance to avoid repeated loads
_timescale = None

def _get_timescale():
    global _timescale
    if _timescale is None:
        _timescale = load.timescale()
    return _timescale

class Satellite:
    def __init__(self, tle):
        self.name = tle['name']
        self.line1 = tle['line1']
        self.line2 = tle['line2']

        ts = _get_timescale()
        self.skyfield_sat = EarthSatellite(self.line1, self.line2, self.name, ts)
        self.norad_id = self.skyfield_sat.model.satnum

        # State Cache
        self.az = 0.0
        self.el = 0.0
        self.range = 0.0
        self.lat = 0.0
        self.lon = 0.0
        self.alt_km = 0.0
        self.visibility = "NO" # YES, DAY, NO
        self.trail = [] # List of [lat, lon]
        self.next_event = "N/A"
        self.apogee = self._calculate_apogee() # Calculated from TLE orbital elements

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

    def update_position(self, observer, t_now, trail_mins=0):
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

        # 2. Geodetic Position (Lat/Lon/Alt)
        geocentric = self.skyfield_sat.at(t)
        subpoint = wgs84.subpoint(geocentric)
        self.lat = subpoint.latitude.degrees
        self.lon = subpoint.longitude.degrees
        self.alt_km = subpoint.elevation.km

        # 3. Visibility Logic
        # VISIBLE = Sunlit Satellite AND Dark Observer
        # DAYLIGHT = Sunlit Satellite AND Sunlit Observer (or just Sunlit Satellite?)
        # NO = Eclipsed Satellite (or below horizon, but this method is state-agnostic of horizon for now)

        sat_sunlit = geocentric.is_sunlit(observer.eph)
        obs_sunlit = observer.is_sunlit(t)

        if not sat_sunlit:
            self.visibility = "NO"
        elif not obs_sunlit:
            self.visibility = "YES" # Visual pass!
        else:
            self.visibility = "DAY" # Radio pass

        # 4. Trail Calculation (Ground Track)
        if trail_mins > 0:
            self.trail = self.calculate_ground_track(t, trail_mins)

        # 5. Apogee is computed once from TLE in __init__, no need to update here

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
