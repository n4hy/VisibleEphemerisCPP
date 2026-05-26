"""Ground-station model for the Python tracker.

Holds the observer's geodetic location (plain lat/lon/alt plus a Skyfield
wgs84 site) and provides Skyfield-based look angles, solar altitude, and Sun
position used for visibility classification. The plain coordinates are consumed
by the HPOP backend's topocentric geometry. Functional twin of the C++ Observer.
"""
import datetime
from skyfield.api import load, wgs84, Time

# Singleton ephemeris to avoid loading 16MB file multiple times
_ephemeris = None
_timescale = None

def _get_ephemeris():
    global _ephemeris
    if _ephemeris is None:
        _ephemeris = load('de421.bsp')
    return _ephemeris

def _get_timescale():
    global _timescale
    if _timescale is None:
        _timescale = load.timescale()
    return _timescale

class Observer:
    def __init__(self, lat_deg, lon_deg, alt_km):
        self.ts = _get_timescale()
        # Plain coordinates (used by the HPOP backend's topocentric geometry).
        self.lat_deg = lat_deg
        self.lon_deg = lon_deg
        self.alt_km = alt_km
        # Use wgs84 for better compatibility with modern skyfield features if needed,
        # but Topos is fine for look angles.
        self.location = wgs84.latlon(lat_deg, lon_deg, elevation_m=alt_km * 1000)
        self.eph = _get_ephemeris()  # Shared singleton ephemeris

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

    def sun_altitude_deg(self, dt=None):
        # Apparent altitude of the Sun above the observer's horizon (degrees).
        # Used for observer-darkness classification. NOTE: Skyfield's is_sunlit()
        # is intended for satellites and is degenerate for points on the surface,
        # so we compute the topocentric solar altitude directly instead.
        t = self._ensure_time(dt)
        sun = self.eph['sun']
        earth = self.eph['earth']
        alt, _az, _d = (earth + self.location).at(t).observe(sun).apparent().altaz()
        return alt.degrees

    def get_sun_position_eci(self, dt=None):
        """Get sun position in GCRS/ECI coordinates (km)."""
        t = self._ensure_time(dt)
        sun = self.eph['sun']
        earth = self.eph['earth']
        # Get geocentric position of sun
        sun_pos = earth.at(t).observe(sun).position.km
        return sun_pos

if __name__ == '__main__':
    observer = Observer(lat_deg=39.0, lon_deg=-76.8, alt_km=0.045)
    print(f"Sun Position: {observer.get_sun_position()}")
