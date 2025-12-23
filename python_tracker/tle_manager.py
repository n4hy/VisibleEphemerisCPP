import os
import requests
import time

class TLEManager:
    CELESTRAK_URL = "https://celestrak.org/NORAD/elements/gp.php?GROUP={group}&FORMAT=tle"

    def __init__(self, cache_dir="./tle_cache"):
        self.cache_dir = cache_dir
        if not os.path.exists(self.cache_dir):
            os.makedirs(self.cache_dir)

    def _get_tle_from_cache(self, group):
        cache_file = os.path.join(self.cache_dir, f"{group}.txt")
        if os.path.exists(cache_file) and (time.time() - os.path.getmtime(cache_file)) < 86400:
            with open(cache_file, 'r') as f:
                return f.read()
        return None

    def _save_tle_to_cache(self, group, data):
        cache_file = os.path.join(self.cache_dir, f"{group}.txt")
        with open(cache_file, 'w') as f:
            f.write(data)

    def _download_tle(self, group):
        url = self.CELESTRAK_URL.format(group=group)
        try:
            response = requests.get(url, timeout=10)
            response.raise_for_status()
            return response.text
        except requests.exceptions.RequestException as e:
            print(f"Error downloading TLE for group {group}: {e}")
            return None

    def _parse_tle(self, tle_data):
        tles = []
        lines = tle_data.strip().splitlines()
        for i in range(0, len(lines), 3):
            name = lines[i].strip()
            line1 = lines[i+1].strip()
            line2 = lines[i+2].strip()
            tles.append({'name': name, 'line1': line1, 'line2': line2})
        return tles

    def load_groups(self, groups_str):
        groups = [g.strip() for g in groups_str.split(',')]
        all_tles = []
        for group in groups:
            tle_data = self._get_tle_from_cache(group)
            if not tle_data:
                tle_data = self._download_tle(group)
                if tle_data:
                    self._save_tle_to_cache(group, tle_data)

            if tle_data:
                all_tles.extend(self._parse_tle(tle_data))
        return all_tles

if __name__ == '__main__':
    # Example usage
    tle_manager = TLEManager()
    tles = tle_manager.load_groups("amateur,weather")
    if tles:
        print(f"Loaded {len(tles)} TLEs.")
        for tle in tles[:5]:
            print(tle)
    else:
        print("Failed to load TLEs.")