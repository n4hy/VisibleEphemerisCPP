from skyfield.api import load
ts = load.timescale()
t = ts.now()
print("Time object created")
try:
    if t is not None:
        print("Time is not None")
except Exception as e:
    print(f"Error checking truthiness: {e}")
