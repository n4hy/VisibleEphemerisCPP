import datetime
from skyfield.api import load, Topos, wgs84, Time
from satellite import Satellite
from tle_manager import TLEManager

class Observer:
    def __init__(self, lat_deg, lon_deg, alt_km):
        self.ts = load.timescale()
        # Use wgs84 for better compatibility with modern skyfield features if needed,
        # but Topos is fine for look angles.
        self.location = wgs84.latlon(lat_deg, lon_deg, elevation_m=alt_km * 1000)
        self.eph = load('de421.bsp') # Needed for Sun/Moon

    def _ensure_time(self, dt):
        if isinstance(dt, Time):
            return dt
        if dt is None:
            dt = datetime.datetime.now(datetime.timezone.utc)
        return self.ts.from_datetime(dt)

    def calculate_look_angle(self, satellite, dt=None):
        t = self._ensure_time(dt)
        difference = satellite.skyfield_sat - self.location
        topocentric = difference.at(t)
        el, az, r = topocentric.altaz()
        return az.degrees, el.degrees, r.km

    def get_sun_position(self, dt=None):
        t = self._ensure_time(dt)
        sun = self.eph['sun']
        earth = self.eph['earth']

        # Geocentric position for the map terminator
        # We need the sub-solar point
        subpoint = wgs84.subpoint(earth.at(t).observe(sun))
        return subpoint.latitude.degrees, subpoint.longitude.degrees

    def is_sunlit(self, dt=None):
        # Check if observer is in sunlight (for day/night visual filtering)
        t = self._ensure_time(dt)
        sun = self.eph['sun']
        return self.location.at(t).is_sunlit(self.eph)

if __name__ == '__main__':
    observer = Observer(lat_deg=39.0, lon_deg=-76.8, alt_km=0.045)
    print(f"Sun Position: {observer.get_sun_position()}")
