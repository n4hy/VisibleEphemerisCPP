from skyfield.api import load, EarthSatellite, wgs84
import datetime
import math

class Satellite:
    def __init__(self, tle):
        self.name = tle['name']
        self.line1 = tle['line1']
        self.line2 = tle['line2']

        ts = load.timescale()
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
        self.apogee = 0.0 # Approximate

    def update_position(self, observer, t_now, trail_mins=0):
        # Ensure t_now is a Skyfield Time object
        ts = load.timescale()
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

        # 5. Approx Apogee/Perigee
        # Using current altitude for now
        self.apogee = self.alt_km

    def calculate_ground_track(self, t_center, minutes):
        # Generate points +/- minutes
        ts = load.timescale()
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

        for i in range(len(lats)):
            points.append([lats[i], lons[i]])

        return points
