"""Utility: pre-download Skyfield data files (timescale + DE421 ephemeris).

Run once so the first tracker launch is not blocked on network fetches.
"""
from skyfield.api import load

print("Attempting to pre-cache Skyfield data...")

# This will download the timescale data files if they are missing.
ts = load.timescale()
print("Timescale data loaded/cached.")

# This will download a planetary ephemeris if it's missing.
# Skyfield often defaults to de421.bsp, which is about 17 MB.
eph = load('de421.bsp')
print("Ephemeris data loaded/cached.")

print("Pre-caching complete.")
