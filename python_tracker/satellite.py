from skyfield.api import load, EarthSatellite
import datetime

class Satellite:
    def __init__(self, tle):
        self.name = tle['name']
        self.line1 = tle['line1']
        self.line2 = tle['line2']

        ts = load.timescale()
        self.skyfield_sat = EarthSatellite(self.line1, self.line2, self.name, ts)
        self.norad_id = self.skyfield_sat.model.satnum

if __name__ == '__main__':
    from tle_manager import TLEManager

    tle_manager = TLEManager()
    tles = tle_manager.load_groups("stations")

    if tles:
        iss_tle = next((tle for tle in tles if "ISS (ZARYA)" in tle['name']), None)
        if iss_tle:
            iss = Satellite(iss_tle)
            print(f"Successfully created Satellite object for: {iss.name} ({iss.norad_id})")
            print(f"Epoch: {iss.skyfield_sat.epoch.utc_iso()}")
        else:
            print("ISS TLE not found in the loaded groups.")
    else:
        print("Failed to load TLEs.")
