import datetime
from skyfield.api import load, Topos
from satellite import Satellite
from tle_manager import TLEManager

class Observer:
    def __init__(self, lat_deg, lon_deg, alt_km):
        self.ts = load.timescale()
        self.location = Topos(latitude_degrees=lat_deg, longitude_degrees=lon_deg, elevation_m=alt_km * 1000)

    def calculate_look_angle(self, satellite, dt=None):
        if dt is None:
            dt = datetime.datetime.now(datetime.timezone.utc)

        t = self.ts.utc(dt)

        difference = satellite.skyfield_sat - self.location
        topocentric = difference.at(t)

        el, az, r = topocentric.altaz()

        return az.degrees, el.degrees, r.km

if __name__ == '__main__':
    observer = Observer(lat_deg=39.0, lon_deg=-76.8, alt_km=0.045)

    tle_manager = TLEManager()
    tles = tle_manager.load_groups("stations")

    if tles:
        iss_tle = next((tle for tle in tles if "ISS (ZARYA)" in tle['name']), None)
        if iss_tle:
            iss = Satellite(iss_tle)
            now = datetime.datetime.now(datetime.timezone.utc)

            az, el, r = observer.calculate_look_angle(iss, now)

            print(f"Observer Location: Lat {observer.location.latitude.degrees:.2f}, Lon {observer.location.longitude.degrees:.2f}")
            print(f"Tracking: {iss.name}")
            print(f"Time (UTC): {now}")

            if az is not None:
                print(f"Azimuth: {az:.2f} deg")
                print(f"Elevation: {el:.2f} deg")
                print(f"Range: {r:.2f} km")
            else:
                print("Satellite propagation failed.")
        else:
            print("ISS TLE not found.")
    else:
        print("Failed to load TLEs.")
