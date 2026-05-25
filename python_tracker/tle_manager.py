import os
import datetime
import hashlib
import configparser
import sys
import requests
import time

# Space-Track endpoints (historical TLE source, gp_history)
SPACETRACK_LOGIN_URL = "https://www.space-track.org/ajaxauth/login"
SPACETRACK_QUERY_BASE = "https://www.space-track.org/basicspacedata/query/class/gp_history"

# Hardcoded NORAD ID range for Iridium-NEXT (launches 2017-01 through 2019-01).
# Used to guarantee Iridium-NEXT coverage even for historical dates when the current
# Celestrak group listing has since dropped decommissioned satellites.
IRIDIUM_NEXT_NORAD_RANGE = range(41917, 43479)


def _credentials_help_text():
    return ("Space-Track credentials not found. Set env vars SPACETRACK_USER / "
            "SPACETRACK_PASS, or create ~/.config/visible-ephemeris/spacetrack.ini "
            "with a [spacetrack] section containing username and password. "
            "Register a free account at https://www.space-track.org/auth/createAccount")


def _load_spacetrack_credentials():
    user = os.environ.get("SPACETRACK_USER", "")
    pwd = os.environ.get("SPACETRACK_PASS", "")
    if user and pwd:
        return user, pwd

    home = os.environ.get("HOME", "")
    ini_path = os.path.join(home, ".config", "visible-ephemeris", "spacetrack.ini")
    if os.path.exists(ini_path):
        cp = configparser.ConfigParser()
        try:
            cp.read(ini_path)
            if cp.has_section("spacetrack"):
                user = user or cp.get("spacetrack", "username", fallback="")
                pwd = pwd or cp.get("spacetrack", "password", fallback="")
        except configparser.Error as e:
            print(f"[SPACETRACK] Failed to parse {ini_path}: {e}", file=sys.stderr)

    return user, pwd


def _fetch_historical_tles(norad_ids, target_date, window_days, dest_path):
    """Fetch historical TLEs from Space-Track gp_history for the given NORAD IDs.

    Writes a 3LE file at dest_path containing one TLE per satellite: the latest EPOCH
    within [target_date - window_days, target_date] (never after the target instant).
    Returns number written.
    """
    user, pwd = _load_spacetrack_credentials()
    if not user or not pwd:
        print(f"[SPACETRACK] {_credentials_help_text()}", file=sys.stderr)
        return 0

    if not norad_ids:
        return 0

    session = requests.Session()
    session.headers.update({"User-Agent": "VisibleEphemeris/spacetrack"})

    print("[SPACETRACK] Logging in...")
    try:
        r = session.post(SPACETRACK_LOGIN_URL,
                         data={"identity": user, "password": pwd},
                         timeout=30)
    except requests.exceptions.RequestException as e:
        print(f"[SPACETRACK] Login network error: {e}", file=sys.stderr)
        return 0
    if r.status_code >= 400:
        print(f"[SPACETRACK] Login failed (HTTP {r.status_code}): {r.text[:200]}", file=sys.stderr)
        return 0
    if "failed" in r.text.lower() and "login" in r.text.lower():
        print(f"[SPACETRACK] Authentication rejected: {r.text[:200]}", file=sys.stderr)
        return 0

    if isinstance(target_date, datetime.datetime):
        target_dt = target_date.astimezone(datetime.timezone.utc) if target_date.tzinfo \
            else target_date.replace(tzinfo=datetime.timezone.utc)
    else:
        target_dt = datetime.datetime.combine(target_date, datetime.time(12, 0),
                                              tzinfo=datetime.timezone.utc)

    start_iso = (target_dt - datetime.timedelta(days=window_days)).strftime("%Y-%m-%dT%H:%M:%S")
    # End at the target instant — never select an element set published after the target.
    end_iso = target_dt.strftime("%Y-%m-%dT%H:%M:%S")
    id_list = ",".join(str(i) for i in norad_ids)

    url = (f"{SPACETRACK_QUERY_BASE}/NORAD_CAT_ID/{id_list}"
           f"/EPOCH/{start_iso}--{end_iso}"
           f"/orderby/NORAD_CAT_ID,EPOCH%20desc/format/3le")

    print(f"[SPACETRACK] Querying gp_history for {len(norad_ids)} IDs "
          f"around {target_dt.strftime('%Y-%m-%dT%H:%M:%SZ')} ...")
    try:
        r = session.get(url, timeout=90)
    except requests.exceptions.RequestException as e:
        print(f"[SPACETRACK] Query network error: {e}", file=sys.stderr)
        return 0
    if r.status_code >= 400:
        print(f"[SPACETRACK] Query failed (HTTP {r.status_code}): {r.text[:200]}", file=sys.stderr)
        return 0
    if not r.text.strip():
        print("[SPACETRACK] Empty response (no matching TLEs in window).", file=sys.stderr)
        return 0

    # Filter: keep only the first (latest) TLE per NORAD ID from the ordered stream.
    lines = [ln.rstrip("\r\n") for ln in r.text.splitlines()]
    seen_ids = set()
    kept = 0
    with open(dest_path, "w") as out:
        i = 0
        while i + 2 < len(lines):
            name = lines[i].strip()
            l1 = lines[i + 1].strip()
            l2 = lines[i + 2].strip()
            if not (l1.startswith("1 ") and l2.startswith("2 ") and len(l1) >= 7):
                i += 1  # resync
                continue
            try:
                nid = int(l1[2:7])
            except ValueError:
                i += 3
                continue
            if nid not in seen_ids:
                seen_ids.add(nid)
                out.write(f"{name}\n{l1}\n{l2}\n")
                kept += 1
            i += 3

    print(f"[SPACETRACK] Retrieved {kept} TLEs (one per satellite).")
    return kept


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

    def load_specific_sats(self, sat_names_csv):
        """Select TLEs whose name matches any comma-separated substring (case-insensitive)
        from the current Celestrak active.txt catalog. Mirrors the C++ path."""
        targets = [t.strip().upper() for t in sat_names_csv.split(",") if t.strip()]
        if not targets:
            return []
        active = self._get_tle_from_cache("active")
        if not active:
            active = self._download_tle("active")
            if active:
                self._save_tle_to_cache("active", active)
        if not active:
            return []
        out = []
        parsed = self._parse_tle(active)
        for tle in parsed:
            name_u = tle["name"].upper()
            if any(t in name_u for t in targets):
                out.append(tle)
        return out

    # ===== Historical TLE support =====

    def _historical_cache_path(self, key, target_date):
        """Return path under ./tle_cache/historical/<YYYY-MM-DD>/<key>.txt"""
        if isinstance(target_date, datetime.datetime):
            d = target_date.astimezone(datetime.timezone.utc).date()
        else:
            d = target_date
        hist_dir = os.path.join(self.cache_dir, "historical", d.strftime("%Y-%m-%d"))
        os.makedirs(hist_dir, exist_ok=True)
        return os.path.join(hist_dir, f"{key}.txt")

    def _resolve_group_to_norad_ids(self, group):
        """Derive NORAD IDs for a Celestrak group, unioned with hardcoded ranges for
        iridium-NEXT so historical queries keep working even if the current listing
        no longer contains the older satellites."""
        ids = set()
        if group in self.VALID_GROUPS:
            tle_data = self._get_tle_from_cache(group)
            if not tle_data:
                tle_data = self._download_tle(group)
                if tle_data:
                    self._save_tle_to_cache(group, tle_data)
            if tle_data:
                for tle in self._parse_tle(tle_data):
                    try:
                        ids.add(int(tle["line1"][2:7]))
                    except (ValueError, IndexError):
                        pass

        if group == "iridium-NEXT":
            ids.update(IRIDIUM_NEXT_NORAD_RANGE)

        return sorted(ids)

    def _resolve_sat_names_to_norad_ids(self, targets_upper):
        active = self._get_tle_from_cache("active")
        if not active:
            active = self._download_tle("active")
            if active:
                self._save_tle_to_cache("active", active)
        if not active:
            return []
        ids = []
        for tle in self._parse_tle(active):
            name_u = tle["name"].upper()
            if any(t in name_u for t in targets_upper):
                try:
                    ids.append(int(tle["line1"][2:7]))
                except (ValueError, IndexError):
                    pass
        return ids

    def load_groups_for_date(self, groups_str, target_date, window_days=10):
        """Fetch historical TLEs valid on target_date for each comma-separated Celestrak
        group. Uses Space-Track gp_history. Cached permanently under
        ./tle_cache/historical/YYYY-MM-DD/<group>.txt."""
        all_tles = []
        seen_ids = set()
        for group in [g.strip() for g in groups_str.split(",")]:
            if not group:
                continue
            cache_path = self._historical_cache_path(group, target_date)
            if not (os.path.exists(cache_path) and os.path.getsize(cache_path) > 0):
                ids = self._resolve_group_to_norad_ids(group)
                if not ids:
                    print(f"[TLE-HIST] No NORAD IDs resolved for group [{group}]", file=sys.stderr)
                    continue
                kept = _fetch_historical_tles(ids, target_date, window_days, cache_path)
                if kept <= 0:
                    print(f"[TLE-HIST] Fetch failed for group [{group}]", file=sys.stderr)
                    continue
            else:
                d_str = os.path.basename(os.path.dirname(cache_path))
                print(f"[CACHE] Historical {group} @ {d_str}")

            with open(cache_path, "r") as f:
                data = f.read()
            for tle in self._parse_tle(data):
                try:
                    nid = int(tle["line1"][2:7])
                except (ValueError, IndexError):
                    continue
                if nid not in seen_ids:
                    seen_ids.add(nid)
                    all_tles.append(tle)
        return all_tles

    def load_specific_sats_for_date(self, sat_names_csv, target_date, window_days=10):
        targets = [t.strip().upper() for t in sat_names_csv.split(",") if t.strip()]
        # SUN and MOON are handled specially in the C++ code but the Python tracker
        # doesn't currently render them synthetically, so we just drop them from
        # historical queries and rely on the normal name-match path returning nothing.
        real = [t for t in targets if t not in ("SUN", "MOON")]
        if not real:
            return []

        key_src = "|".join(sorted(real))
        key = "satsel_" + hashlib.sha1(key_src.encode("utf-8")).hexdigest()[:12]
        cache_path = self._historical_cache_path(key, target_date)

        if not (os.path.exists(cache_path) and os.path.getsize(cache_path) > 0):
            ids = self._resolve_sat_names_to_norad_ids(real)
            if not ids:
                print("[TLE-HIST] No NORAD IDs matched --satsel names.", file=sys.stderr)
                return []
            kept = _fetch_historical_tles(ids, target_date, window_days, cache_path)
            if kept <= 0:
                print("[TLE-HIST] Historical fetch failed for --satsel.", file=sys.stderr)
                return []
        else:
            d_str = os.path.basename(os.path.dirname(cache_path))
            print(f"[CACHE] Historical satsel @ {d_str}")

        with open(cache_path, "r") as f:
            data = f.read()
        return self._parse_tle(data)

if __name__ == '__main__':
    # Example usage
    tle_manager = TLEManager()
    tles = tle_manager.load_groups("amateur,weather")
    if tles:
        print(f"Loaded {len(tles)} TLEs.")
    else:
        print("Failed to load TLEs.")
