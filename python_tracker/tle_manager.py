import os
import requests
import time

class TLEManager:
    CELESTRAK_URL = "https://celestrak.org/NORAD/elements/gp.php?GROUP={group}&FORMAT=tle"

    # Valid Celestrak group names (matching C++ whitelist)
    VALID_GROUPS = {
        # Special
        "active", "visual", "stations", "last-30-days", "analyst",
        # Weather
        "weather", "noaa", "goes", "resource", "sarsat", "dmc", "tdrss", "argos", "planet", "spire",
        # Comm
        "geo", "intelsat", "ses", "iridium", "iridium-NEXT", "starlink", "oneweb", "orbcomm",
        "globalstar", "swpc", "amateur", "x-comm", "other-comm", "satnogs", "gorizont", "raduga", "molniya",
        # Nav
        "gnss", "gps-ops", "glo-ops", "galileo", "beidou", "sbas", "nnss", "musson",
        # Science
        "science", "geodetic", "engineering", "education",
        # Misc
        "military", "radar", "cubesat", "other"
    }

    # Maximum file size for anti-poison protection (2MB, except for active.txt)
    MAX_FILE_SIZE = 2 * 1024 * 1024

    def __init__(self, cache_dir="./tle_cache"):
        self.cache_dir = cache_dir
        if not os.path.exists(self.cache_dir):
            os.makedirs(self.cache_dir)

    def _get_tle_from_cache(self, group):
        cache_file = os.path.join(self.cache_dir, f"{group}.txt")
        # Check if file exists and is less than 24 hours (86400 seconds) old
        if os.path.exists(cache_file):
            # Anti-poison check: file size limit (except for active.txt)
            file_size = os.path.getsize(cache_file)
            if group != "active" and file_size > self.MAX_FILE_SIZE:
                print(f"[CACHE] CORRUPT: File too large for group '{group}' ({file_size} bytes). Deleting.")
                os.remove(cache_file)
                return None
            if file_size == 0:
                print(f"[CACHE] Empty cache file for '{group}'. Deleting.")
                os.remove(cache_file)
                return None

            age = time.time() - os.path.getmtime(cache_file)
            if age < 86400:
                print(f"[CACHE] Found fresh data for '{group}' (Age: {age/3600:.1f}h)")
                with open(cache_file, 'r') as f:
                    return f.read()
            else:
                print(f"[CACHE] Expired data for '{group}' (Age: {age/3600:.1f}h). Reloading.")
        return None

    def _save_tle_to_cache(self, group, data):
        cache_file = os.path.join(self.cache_dir, f"{group}.txt")
        with open(cache_file, 'w') as f:
            f.write(data)

    def _download_tle(self, group):
        print(f"[NET] Downloading TLE for '{group}'...")
        url = self.CELESTRAK_URL.format(group=group)
        try:
            response = requests.get(url, timeout=10)
            response.raise_for_status()
            print(f"[NET] Download successful ({len(response.text)} bytes)")
            return response.text
        except requests.exceptions.RequestException as e:
            print(f"[ERR] Error downloading TLE for group {group}: {e}")
            return None

    def _parse_tle(self, tle_data):
        tles = []
        lines = tle_data.strip().splitlines()
        # Ensure we have groups of 3 lines
        for i in range(0, len(lines), 3):
            if i+2 < len(lines):
                name = lines[i].strip()
                line1 = lines[i+1].strip()
                line2 = lines[i+2].strip()
                # Basic validation
                if line1.startswith('1 ') and line2.startswith('2 '):
                    tles.append({'name': name, 'line1': line1, 'line2': line2})
        return tles

    def load_groups(self, groups_str):
        groups = [g.strip() for g in groups_str.split(',')]
        all_tles = []
        for group in groups:
            # Validate group name (matching C++ whitelist behavior)
            if group not in self.VALID_GROUPS:
                print(f"[ERROR] Unknown Group Name: [{group}]. Skipping.")
                continue

            tle_data = self._get_tle_from_cache(group)

            if not tle_data:
                tle_data = self._download_tle(group)
                if tle_data:
                    self._save_tle_to_cache(group, tle_data)

            if tle_data:
                parsed = self._parse_tle(tle_data)
                all_tles.extend(parsed)
            else:
                # If download failed, try to load stale cache as fallback?
                # For now, just warn.
                pass

        return all_tles

if __name__ == '__main__':
    # Example usage
    tle_manager = TLEManager()
    tles = tle_manager.load_groups("amateur,weather")
    if tles:
        print(f"Loaded {len(tles)} TLEs.")
    else:
        print("Failed to load TLEs.")
