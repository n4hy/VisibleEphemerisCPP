# Visible Ephemeris
### High-Performance Satellite Tracking Appliance (C++17 & Python)

**Visible Ephemeris** is a modern, spiritual successor to *Quiktrak* (1986), re-engineered for the Raspberry Pi 5 and modern silicon. It is capable of propagating 13,000+ satellites in real-time with sub-second updates while maintaining <5% CPU utilization.

It features a **Hybrid Decoupled Architecture** where the UI, Orbital Mechanics, and Network Services run on independent threads, ensuring the interface never freezes—even during heavy calculation loads.

Both **C++** and **Python** implementations are provided with identical functionality.

---

## Core Features

### Tracking Engine
* **SGP4/SDP4 Propagation**: Uses `libsgp4` (C++) or `Skyfield` (Python) for high-precision orbital math.
* **High-Precision Orbit Propagator (HPOP)**: Optional `--hpop` mode that numerically integrates a full force model — EGM96 gravity (degree/order 10 by default, up to 20), Sun/Moon third-body, atmospheric drag, and solar radiation pressure — with an adaptive Fehlberg RK7(8) integrator, seeded from the TLE state vector at epoch. Available in both C++ and Python (via the `ve_hpop` pybind11 module). See [High-Precision Orbit Propagator](#high-precision-orbit-propagator-hpop).
* **Massive Scale**: Tracks the entire NORAD Active Catalog (13,000+ objects) simultaneously.
* **Smart Caching**: Automatic TLE downloading and caching from Celestrak with 24-hour auto-refresh cycle. Historical TLEs (`tle_cache/historical/YYYY-MM-DD/`) are cached permanently since archived elements never change.
* **Multi-Group Selection**: Track specific combinations (e.g., `amateur,weather,stations`) using the `group_selection` config or `--groupsel` argument.
* **Stability**: Implements "Pre-calculate All" logic at startup to ensure 24-hour pass predictions are instantly available, eliminating "Calculating..." flicker and UI jitter.
* **Decoupled Clock**: Simulation time input is treated as "Face Value" (Local Wall-Clock Time) for display, while strictly adhering to UTC for orbital physics, eliminating timezone confusion.
* **Historical Playback**: Run the tracker at any past UTC date. For dates >24 h in the past, TLEs valid on that date are pulled from Space-Track.org's `gp_history` archive, cached permanently, and propagated exactly as the live tracker does. Full Iridium-NEXT constellation coverage from its 2017–2019 launches. See [Historical Tracking](#historical-tracking-past-dates).
* **Pluggable Earth-Rotation Model** (`include/earth_rotation.hpp`): a `Rotation3` /
  `IEarthRotation` interface lets the visibility and display code rotate
  ECI → ECEF through either the legacy **`GmstRotation`** (GMST-only z-rotation,
  the default, equivalent to the original two-line code) or the
  **`IAU2000Rotation`** which uses GAST = GMST + Equation of the Equinoxes
  (IAU 1980 nutation). Since SGP4 outputs are in TEME (not GCRS), the
  IAU2000 rotation is the correct reduction for SGP4 → ITRS and removes
  the ~1.3 arc-second equation-of-equinoxes bias from sub-satellite
  points and similar geometry. The Python tracker already inherits the
  full IAU 2006/2000A chain implicitly through Skyfield's `.at(t)` /
  `wgs84.subpoint()` calls in `python_tracker/satellite.py` and
  `observer.py`.

### Display Systems
* **NCurses Terminal Dashboard** (C++):
    * Flicker-free, color-coded real-time data table.
    * **Flare Detection**: Identifies specular reflections from LEO satellites (flashing 'F' indicator).
    * **Horizon Flash**: Visual indicator (Red/White flashing) when a satellite is rising or setting (within 1° of horizon).
* **Console Output** (Python):
    * Real-time updating satellite table with visibility status.
* **Web Dashboard (Port 8080)**:
    * **Mercator Map**: Live ground tracks, observer location marker, solar terminator, and satellite footprint visualization.
    * **Polar Skyplot**: Radar view of visible satellites with pulsing selection aura.
    * **Smart Trails**: Displays ground track history and future path.
    * **Sortable Table**: Click column headers to sort by Name, Azimuth, Elevation, etc.
* **Web Terminal Mirror (Port 12345)**:
    * Ultra-lightweight HTML mirror of the terminal output.
    * Uses HTTP/1.0 "Fire-and-Forget" protocol to prevent browser hanging on slow connections.
* **Qt 6 Desktop Dashboards** (PyQt6 ≥ 6.4, VS Code Dark+ palette):
    * **`ve-ide`** — VS Code-style IDE for the project. Activity bar, file
      tree, tabbed editor with Python/C++/YAML syntax highlighting, embedded
      terminal, quick-open (`Ctrl+P`), command palette (`Ctrl+Shift+P`),
      one-click Run buttons for the tracker / CMake / ctest, and a live
      Satellites tab that streams the tracker's physics feed inline.
    * **`ve-orbital-architect-qt`** — Ergonomic satellite fleet picker.
      Live-filter catalog, checkbox multi-select, station config, and Deploy
      button that writes `tle_cache/<group>.txt` + `config.yaml` in one shot.
    * **`ve-hpop-panel`** — Real-time HPOP / physics-stream monitor.
      Auto-reconnecting TCP client on `:12346`, sortable fleet table, and
      pyqtgraph strip charts of elevation / range / range-rate / visible-count
      for the selected satellite. See [Dashboards guide](docs/dashboards.md).

---

## Operating Modes

### Radio Mode (`visible_only: false`)
Shows **ALL satellites** in the selected group(s), color-coded by elevation and visibility:

| Color | Condition | Description |
|:------|:----------|:------------|
| **Yellow** | Above min_el AND Visible | Naked-eye visible: above the horizon, observer in astronomical twilight or darker (sun ≤ −12°), and satellite sunlit (not in Earth's shadow) |
| **Green** | Above min_el AND NOT Visible | Above minimum elevation but not naked-eye visible (daylight/twilight, or satellite eclipsed) - good for radio |
| **Grey** | Below min_el OR Below Horizon | Satellite is low or not yet risen - displayed for situational awareness |

This mode displays every satellite in the group on the map and in tables, limited only by `max_sats`.

### Naked-eye visibility definition
A satellite is reported **Visible** (yellow) when **all** of the following hold:
1. It is **above the observer's horizon** (elevation > 0°).
2. The observer is in **astronomical twilight or darker** — the Sun is at or below **−12°** altitude.
3. The satellite is **sunlit** — not inside Earth's shadow.

Otherwise a sunlit satellite is reported as daylight/not-visible, and a shadowed one as eclipsed. This definition is identical in the C++ and Python implementations.

### Optical Mode (`visible_only: true`)
Shows only satellites that meet the naked-eye visibility definition above (and are above `min_el`). This optional mode is for visual observers who only want satellites they can actually spot. With the default `visible_only: false`, every satellite above `min_el` is displayed and color coding alone distinguishes visibility.

### Hardware Control
* **Radio Control**: Automated Hamlib control for Transceiver Frequency/Mode (Doppler correction). *Requires single satellite selection.*
* **Rotator Control**: Automated Hamlib control for Azimuth/Elevation tracking. *Requires single satellite selection.*

---

## Installation

### One-line install (Python tracker + Qt dashboards)

From a checkout, install everything the Python tracker and Qt dashboards need
and put four launchers on your `PATH`:

```bash
./install.sh                # user install into ~/.local (no sudo)
./install.sh --system       # system install into /usr/local (needs sudo)
```

After it finishes:

| Launcher                    | Runs                                        |
| --------------------------- | ------------------------------------------- |
| `ve-ide`                    | VS Code-style Qt IDE dashboard              |
| `ve-orbital-architect-qt`   | Qt satellite fleet selector                 |
| `ve-hpop-panel`             | Qt physics-stream monitor                   |
| `ve-tracker`                | CLI + web + text tracker (`python_tracker.main`) |

The install is idempotent; roll it back with `./install.sh --uninstall`.

For sysadmin fleets, build a native Debian package instead:

```bash
sudo apt-get install -y debhelper dpkg-dev fakeroot rsync
packaging/build_deb.sh --clean --install
```

See [`docs/installation.md`](docs/installation.md) for full details (flags,
apt-package list, non-Debian distros, uninstall) and
[`docs/dashboards.md`](docs/dashboards.md) for the dashboards guide.

### C++ Version (Primary)

We provide an automated build script `build.sh` that handles dependencies (including building `libsgp4` from source) and installation.

```bash
cd VisibleEphemeris
chmod +x build.sh
./build.sh
```

**Note:** The script utilizes `sudo` to install dependencies and the final binary.

#### Manual Build

Dependencies (Ubuntu/Debian): `cmake` (≥ 3.14), a C++17 compiler (`clang` or `g++`), `libncurses-dev`, `libcurl4-openssl-dev`, and `pkg-config`. The SGP4 propagation library `libsgp4` must also be present — `build.sh` builds it from [dnwrnr/sgp4](https://github.com/dnwrnr/sgp4). `libhamlib-dev` is optional and enables radio/rotator control.

```bash
sudo apt install cmake clang libncurses-dev libcurl4-openssl-dev pkg-config libhamlib-dev

mkdir build && cd build
cmake ..                 # auto-selects clang if present, else GCC; add -DENABLE_HAMLIB=OFF to skip Hamlib
make -j$(nproc)
```

Optional CMake switches:

| Option | Default | Effect |
|:--|:--|:--|
| `-DENABLE_HAMLIB=OFF` | ON | Skip Hamlib radio/rotator support |
| `-DBUILD_PYTHON_BINDINGS=ON` | OFF | Build the `ve_hpop` pybind11 module (see [HPOP](#high-precision-orbit-propagator-hpop)) |
| `-DBUILD_TESTS=ON` | OFF | Build the unit tests; run with `ctest` |

If `libsgp4` is installed to a non-standard prefix (e.g. `~/sgp4/build/install`, where `build.sh` puts it), put it on the library path at runtime:
```bash
LD_LIBRARY_PATH=~/sgp4/build/install/lib ./VisibleEphemeris
```

### Python Version

The Python tracker is located in `python_tracker/` and provides identical functionality.

**Prerequisites:**
* Python 3.10+
* Linux (macOS/BSD work for the tracker; the terminal keyboard poller no-ops
  on Windows and under non-TTY parents such as `ve-ide`'s Run buttons)

**Installation (venv, self-contained):**
```bash
python3 -m venv venv
./venv/bin/pip install -r python_tracker/requirements.txt
```

That single `pip install` now pulls **everything** the tracker and the Qt
dashboards need — PyQt6 (with bundled Qt), pyqtgraph, matplotlib, skyfield,
fastapi, uvicorn, numpy, PyYAML, requests, pybind11.

**Running:**
```bash
# From repo root (recommended — matches ve-ide's Run buttons)
./venv/bin/python3 -m python_tracker.main
./venv/bin/python3 -m python_tracker.main --hpop

# From python_tracker/ (legacy)
cd python_tracker && ../venv/bin/python3 main.py
```

`main.py` supports both invocation styles and detects non-TTY stdin (skipping
the interactive keyboard poller and the on-exit save prompt) so it runs cleanly
under `ve-ide`, systemd, cron, or any other non-terminal parent.

---

## Configuration

Settings are stored in `config.yaml` and automatically saved on exit. Both C++ and Python versions use the same configuration format.

### Configuration File (`config.yaml`)
```yaml
lat: 39.6478              # Observer Latitude (decimal degrees)
lon: -76.1347             # Observer Longitude (decimal degrees)
alt: 0.075                # Observer Altitude (km)
max_sats: 100             # Maximum satellites to display
min_el: 0                 # Minimum elevation filter (degrees)
max_apo: -1               # Maximum apogee filter, -1 = disabled
trail_length_mins: 5      # Ground track trail length (+/- minutes)
group_selection: iridium-NEXT   # Celestrak group(s), comma-separated
sat_selection: ""         # Specific satellite names (overrides group_selection). e.g. "ISS,NOAA 19"
visible_only: false       # false = Radio Mode (all sats), true = Optical Mode
delta_t: 1.0              # Update interval in seconds (0.001-60)
radio_control: false      # Enable Hamlib radio control (Doppler)
rotator_control: false    # Enable Hamlib rotator control
rotator_host: localhost   # Rotator daemon host
rotator_port: 4533        # Rotator daemon port
rotator_min_el: 0         # Minimum elevation for rotator tracking
```

> **Note:** Space-Track credentials are **not** stored in `config.yaml`. They live outside the repo in environment variables or `~/.config/visible-ephemeris/spacetrack.ini` — see [Space-Track credentials](#space-track-credentials).

### Command Line Arguments

Both C++ and Python accept the same flag names unless noted.

| Argument | Description | Default |
|:---------|:------------|:--------|
| `--help`, `-h` | Print the full option list and network-port summary, then exit | - |
| `--lat <deg>` | Observer Latitude (Decimal Degrees) | from config |
| `--lon <deg>` | Observer Longitude (Decimal Degrees) | from config |
| `--alt <km>` | Observer Altitude (km) | from config |
| `--groupsel <list>` | Comma-separated Celestrak groups (e.g., `amateur,weather`) | `active` |
| `--satsel <list>` | Comma-separated satellite names (substring match); overrides `--groupsel` | from config |
| `--visible` | Optical Mode: show only sunlit satellites | from config |
| `--no-visible` | Radio Mode: show ALL satellites (color-coded by visibility) | - |
| `--minel <deg>` | Minimum elevation filter | 0.0 |
| `--maxsats <N>` | Maximum satellites to display | 100 |
| `--maxapo <km>` | Filter satellites with apogee > N km | -1 (disabled) |
| `--trail_mins <N>` | Ground track trail length (+/- minutes) | 5 |
| `--radio <bool>` | Enable Hamlib radio control (C++ only, requires single `--satsel`) | false |
| `--rotator <bool>` | Enable Hamlib rotator control (C++ only, requires single `--satsel`) | false |
| `--refresh` | Force fresh download of TLE data (C++ only) | false |
| `--time <str>` | Simulate time in UTC (`"YYYY-MM-DD HH:MM:SS"`). Past dates >24 h ago trigger historical TLE retrieval from Space-Track — see [Historical Tracking](#historical-tracking-past-dates) | Real-time |
| `--deltaT <sec>` | Update interval in seconds (0.001-60) | 1.0 |
| `--hpop` | Use the High-Precision Orbit Propagator instead of SGP4 (see [below](#high-precision-orbit-propagator-hpop)) | off |
| `--hpop-degree <N>` | HPOP geopotential degree/order (1-20). 10 is sub-100 m/day for LEO at ~half the cost of 20; raise for geodesy-grade work | 10 |
| `--no-drag` | HPOP: disable atmospheric drag | drag on |
| `--no-srp` | HPOP: disable solar radiation pressure | srp on |
| `--no-thirdbody` | HPOP: disable Sun/Moon third-body perturbations | on |
| `--port <A,B,C>` | Override network ports (web,text,physics) — C++ only | 8080,12345,12346 |
| `--groupbuild` | Enter Mission Planner builder mode (C++ only) | - |

---

## High-Precision Orbit Propagator (HPOP)

By default the tracker propagates orbits analytically with **SGP4**. The `--hpop`
option instead **numerically integrates a full force model**, which is more
accurate than SGP4's truncated secular/periodic theory — especially over multi-
hour to multi-day arcs and for non-spherical-gravity-sensitive orbits.

### How the initial state is obtained

A TLE does **not** contain an osculating state vector — lines 1–2 hold *SGP4 mean
elements*. HPOP therefore converts the TLE to a state vector the only correct
way: it evaluates SGP4 **at the element-set epoch** to produce an osculating ECI
(TEME) position/velocity `(r₀, v₀)`, and uses that as the initial condition for
numerical integration. The integration frame is the same TEME-as-pseudo-inertial
frame SGP4 produces, so all downstream geometry (look angles, ground track,
visibility) is unchanged.

### Force model

| Perturbation | Model |
|:--|:--|
| Earth gravity | **EGM96** spherical harmonics (Cunningham/Pines recursion), evaluated Earth-fixed via GMST. Default degree/order 10, selectable up to 20 via `--hpop-degree`. Coefficients embedded in the binary. |
| Third body | Sun and Moon point-mass (Montenbruck-Gill analytic ephemerides) |
| Atmospheric drag | Piecewise-exponential density (Vallado); co-rotating atmosphere. Ballistic coefficient `Cd·A/m = 2·B*/0.15696615` derived from the TLE B* term |
| Solar radiation pressure | `4.56e-6 N/m²` at 1 AU, cylindrical Earth-shadow; `Cr·A/m` shared from the drag area |

Integration uses an **adaptive Fehlberg RK7(8)** scheme (8th-order solution,
embedded 7th-order error control). Because the integrator marches on its own
adaptive step boundaries and lands on each requested epoch with a single exact
partial step, results are independent of how finely you sample — a direct jump
and a fine-grained march to the same time agree to machine precision.
Verification: HPOP at epoch reproduces the SGP4 seed exactly; two-body
semi-major axis is conserved to ~1 cm over a day.

> HPOP is heavier than SGP4. It is best for a focused set of satellites
> (`--satsel`) rather than the full 13,000-object catalog.

### Running

```bash
# C++ — track the ISS with the full high-precision model
LD_LIBRARY_PATH=~/sgp4/build/install/lib ./VisibleEphemeris --satsel ISS --hpop

# Full 20x20 geopotential, gravity + Sun/Moon only (no drag/SRP)
./VisibleEphemeris --satsel ISS --hpop --hpop-degree 20 --no-drag --no-srp

# Python tracker
python3 main.py --satsel ISS --hpop
```

### Python module (pybind11)

The same C++ propagator is exposed to Python as the `ve_hpop` module. Build it
with the `BUILD_PYTHON_BINDINGS` CMake option (pybind11 is located via
`find_package`/pip, or fetched from GitHub if absent):

```bash
cd build
cmake .. -DBUILD_PYTHON_BINDINGS=ON -DPython_EXECUTABLE=$(which python3)
cmake --build . --target ve_hpop      # -> ve_hpop.<abi>.so
```

The Python tracker auto-discovers the module in `../build`; you can also add the
build directory to `PYTHONPATH`. Usage:

```python
import ve_hpop
p = ve_hpop.Propagator(name, line1, line2, degree=10,
                       drag=True, srp=True, thirdbody=True)  # optional mass_kg/area/Cd/Cr
r, v = p.propagate_jd(jd)             # ECI/TEME position (km) & velocity (km/s)
r, v = p.propagate(datetime_utc)      # also accepts a Python datetime
lat, lon, alt = p.geodetic_jd(jd)     # WGS84 sub-satellite point
print(p.epoch_jd, p.cd_area_over_m, p.drag_enabled)
# module helpers: ve_hpop.sun_position_eci(jd), moon_position_eci(jd),
#                 atmosphere_density(alt_km), gmst_rad(jd)
```

### Example: HPOP vs SGP4 over a day

`examples/compare_iss.cpp` propagates the ISS for a full day with both HPOP and
SGP4 and prints the position difference decomposed into the orbital RSW frame
(radial / along-track / cross-track):

```bash
cd build
cmake .. -DBUILD_EXAMPLES=ON && cmake --build . --target compare_iss
LD_LIBRARY_PATH=~/sgp4/build/install/lib ./compare_iss
```

The separation is essentially all along-track (the orbits stay on the same plane
and shape but drift in phase), growing to ~10 km after 24 h - the expected
signature of a full numerical force model diverging from SGP4's analytic theory.
This is a model-difference demonstration, not an accuracy verdict: HPOP is seeded
from SGP4's mean elements at epoch, so neither result is ground truth.

---

## Historical Tracking (Past Dates)

When `--time` specifies a UTC date more than 24 hours in the past, the tracker fetches the TLEs that were current on that date from **Space-Track.org** (endpoint `gp_history`), caches them under `tle_cache/historical/<YYYY-MM-DD>/`, and propagates from there. This avoids the multi-kilometer SGP4 error that would result from propagating today's elements backward years.

Coverage is authoritative for the full USSPACECOM catalog back to 1957, including the entire Iridium-NEXT constellation (NORAD 41917–43478) from its 2017–2019 launches.

### Space-Track credentials

Historical TLE retrieval uses **Space-Track.org**, the authoritative public archive operated by the U.S. 18th Space Defense Squadron. An account is free but required; it is only needed when `--time` selects a date more than 24 hours from wall-clock now. Real-time and near-real-time operation continue to use Celestrak and require no account.

#### Step 1 — Register for a Space-Track account

Open **https://www.space-track.org/auth/createAccount** in your browser and fill out the registration form. The following fields are required:

| Field | Notes |
|:------|:------|
| Email address | Must be valid and active — a verification email is sent here, and it doubles as your login username. |
| Organization and interests | Describe who you are and why you want the data (e.g., "Amateur radio operator; historical satellite visibility analysis with open-source Visible Ephemeris tracker"). A brief, honest description is enough. |
| Name (first / middle / last, optional prefix/suffix) | *"Special characters and numerals are not allowed in names (Dashes, periods, spaces and apostrophes are allowed)."* |
| Phone number | Used only for account-related contact. |
| Mailing address | Street, city, state/region, postal code, country. |

You will be asked to agree to the **User Agreement**. The key obligations are worth reading in full, but in summary:

- You will **not transfer data or technical information received from the site to any other entity without prior express approval**. In practice, TLEs (which fall under "basic SSA data") are allowed to be used in your own tools and redistributed with citation; anything beyond basic data requires explicit authorization.
- You will **not share or transfer your username/password**. Each individual using the data needs their own account.
- Access is **currently free** ("*The present U.S. Government policy is not to charge for website access*") but must be **renewed periodically**.

For Visible Ephemeris, your use — pulling historical TLEs to your own cache for personal propagation — is within the scope of basic SSA data use. Do not check cached TLEs into a public repository or redistribute them in bulk.

#### Step 2 — Confirm your email

After submitting the form, Space-Track sends a confirmation email. Click the link to activate the account. If you don't receive it within a reasonable time or the form rejects your submission, contact **admin@space-track.org**.

#### Step 3 — Verify login on the website

Log in once at https://www.space-track.org/ with your email and chosen password before using the credentials here. This confirms the account is active and lets you read the current API documentation at https://www.space-track.org/documentation.

#### Step 4 — Provide credentials to Visible Ephemeris

Credentials are read at program start, in this order:

1. **Environment variables** (recommended for shell-scripted use):
   ```bash
   export SPACETRACK_USER='your_email@example.com'
   export SPACETRACK_PASS='your_password'
   ```

2. **Config file** `~/.config/visible-ephemeris/spacetrack.ini`:
   ```ini
   [spacetrack]
   username = your_email@example.com
   password = your_password
   ```
   Create the directory and restrict permissions so only your account can read it:
   ```bash
   mkdir -p ~/.config/visible-ephemeris
   chmod 700 ~/.config/visible-ephemeris
   # ... create the file ...
   chmod 600 ~/.config/visible-ephemeris/spacetrack.ini
   ```

Both the C++ and Python implementations check these locations; whichever is found first wins. If neither is set when a historical query is attempted, Visible Ephemeris prints a clear error with these same instructions and exits non-zero — it never silently falls back to current Celestrak TLEs.

#### Step 5 — Mind the rate limits

Space-Track enforces API throttling. The Visible Ephemeris historical loader is designed to consume a tiny fraction of the budget — one `gp_history` range query per (date, group) combination, cached permanently so the second run is offline — but if you script many different dates back-to-back you may hit the limits:

- **General:** *"Limit API queries to less than 30 requests per 1 minute(s) and 300 requests per 1 hour(s)."*
- **Violation:** *"Your space-track account may be suspended if you violate the usage policy."*

One run of Visible Ephemeris over one date and one group = 2 requests (login + query). Each additional date or group name adds one more query.

#### What gp_history gives you

The `gp_history` class on the `basicspacedata` endpoint returns every General Perturbations (GP) element set — including classical TLEs — ever published by USSPACECOM for each object in the public catalog, keyed by EPOCH. This is the same data source CelesTrak and most other TLE services mirror from. Standard registered accounts have access to `gp_history` with no additional approval required.

### Example: Iridium-NEXT on a past date

```bash
# C++
./VisibleEphemeris --time "2019-06-15 12:00:00" --groupsel iridium-NEXT

# Python
python3 main.py --time "2019-06-15 12:00:00" --groupsel iridium-NEXT
```

Expected behavior: the program prints `[SPACETRACK] ...`, writes `tle_cache/historical/2019-06-15/iridium-NEXT.txt`, loads the satellites, and runs normally. Re-running the same command uses the cache with no network call.

Historical TLE selection rule: for each NORAD ID, the tracker keeps the TLE with the latest EPOCH not exceeding the target date, within a ±10 day window. Satellites with no TLE in that window are skipped with a warning.

---

## Usage Examples

### 1. Radio Mode - All Iridium Satellites
Display all Iridium NEXT satellites with elevation-based coloring:
```bash
# C++
./VisibleEphemeris --groupsel iridium-NEXT

# Python
python3 main.py --groupsel iridium-NEXT
```
With `visible_only: false` in config, all satellites appear on the map (yellow/green/grey).

### 2. Optical Mode - Visual Observing
Show only satellites visible to the naked eye (sunlit, observer in darkness):
```bash
# C++
./VisibleEphemeris --groupsel "stations,visual" --visible --minel 10

# Python
python3 main.py --groupsel "stations,visual" --visible --minel 10
```

### 3. Amateur Radio Satellites
Track ham radio satellites above the horizon:
```bash
./VisibleEphemeris --groupsel amateur --minel 0
```

### 4. Specific Location
```bash
./VisibleEphemeris --lat 39.54 --lon -76.09 --alt 0.1
```

### 5. Hardware Control (Single Target)
Track ISS with rotator control:
```bash
./VisibleEphemeris --satsel ISS --rotator
```

### 6. Custom Network Ports
Run on alternative ports (useful for multiple instances or firewall restrictions):
```bash
# Change all three ports
./VisibleEphemeris --port 9080,9345,9346

# Change only the web dashboard port
./VisibleEphemeris --port 9080

# Change only the physics stream port (keep others at defaults)
./VisibleEphemeris --port ,,9346
```

### 7. Historical Playback (Past Date)
Propagate positions as they would have been seen on an arbitrary UTC date. Requires a free Space-Track.org account for dates more than 24 hours in the past (see [Space-Track credentials](#space-track-credentials)):
```bash
# All Iridium-NEXT satellites over a summer day in 2019
./VisibleEphemeris --time "2019-06-15 12:00:00" --groupsel iridium-NEXT

# Track the ISS at a specific moment in 2018
./VisibleEphemeris --time "2018-11-02 03:30:00" --satsel ISS

# Python tracker uses the same flags
python3 main.py --time "2019-06-15 12:00:00" --groupsel iridium-NEXT
```
TLEs are cached permanently under `tle_cache/historical/YYYY-MM-DD/`; re-running the same date is offline.

### 8. Near-Now Simulation (No Credentials Needed)
`--time` within 24 hours of wall-clock now stays on the live Celestrak path and does **not** require Space-Track credentials:
```bash
# 2 hours ago; uses today's cached Celestrak TLEs
./VisibleEphemeris --time "$(date -u -d '2 hours ago' +'%Y-%m-%d %H:%M:%S')" --groupsel stations
```

---

## Planning Tools

Two optional utilities help you assemble a tracking selection before launching
the tracker. Both write to the same `config.yaml` the tracker reads.

### Mission Planner (`--groupbuild`, C++)
```bash
./VisibleEphemeris --groupbuild
```
Instead of starting the tracker, this launches a browser-based **Mission Planner
UI** on the web dashboard port (default 8080; override with `--port`). Open
`http://<IP>:8080` to browse the catalog and build a group. This mode runs
blocking and does not start the ncurses display or the text/physics servers.

### Orbital Architect (`orbital_architect.py`, standalone)
```bash
python3 orbital_architect.py
```
An interactive ANSI-terminal tool that loads the on-disk TLE cache
(`tle_cache/`) into a searchable catalog, lets you browse/search and pick
satellites, and writes the chosen selection plus observer settings to
`config.yaml`. It is a standalone helper, not part of the tracker's runtime
path — run it first, then start the tracker normally.

---

## Keyboard Controls

| Key | Action |
|:----|:-------|
| **Q** | Quit — opens the "Save configuration?" prompt |
| **Y / N / ESC** | At the quit prompt: **Y** save & quit, **N** quit without saving, **ESC** cancel and resume |
| **UP/DOWN** | Scroll satellite list |
| **PAGE UP/DOWN** | Fast scroll (±10 rows) |

---

## Network Services

Visible Ephemeris exposes three network interfaces. Ports can be overridden with the `--port` argument using comma-separated values (e.g., `--port 9000,9001,9002`). Use empty values to keep defaults (e.g., `--port ,,12349` changes only the physics port).

### Graphical Dashboard: `http://<IP>:8080` (default)
* Interactive Mercator map with satellite positions and ground tracks
* Polar skyplot (toggle with MAP/SKY button)
* Sortable satellite table
* Click satellites to select and view details
* Solar terminator visualization
* Satellite footprint radius for selected satellite

### Text Mirror: `http://<IP>:12345` (default)
* Lightweight HTML reflection of terminal output
* Auto-refreshes every second
* Works on low-bandwidth connections

### Physics Stream: `tcp://<IP>:12346` (default)
* **Raw TCP streaming** of physics data (satellite positions, look angles, etc.)
* **Zero-buffer design**: Data is only generated when clients are connected
* **Real-time streaming**: Pushes updates at the rate set by `--deltaT`
* **Frame delimiter**: Each frame ends with `\n---END_FRAME---\n`

**Connecting to Physics Stream:**
```bash
# Using netcat
nc localhost 12346

# Using telnet
telnet localhost 12346

# Using socat (with line buffering)
socat - TCP:localhost:12346
```

**Example Output** (fixed-width columns; rows sorted by elevation, descending):
```
VISIBLE EPHEMERIS v12.65-CODE-ONLY
2026-02-23 12:34:56.7 LOC
OBS: 39.6478, -76.1347 | SHOWN: 2

NAME                  AZ       EL      RANGE RR(km/s) VIS   NEXT EVENT      NORAD        LAT        LON     APOGEE  FLARE
----------------------------------------------------------------------------------------------------------------------
IRIDIUM 140         83.3     29.2     1271.8    0.092 DAY   LOS 6m 42s      43074    41.2317   -72.9841      780.1      0
IRIDIUM 110        243.1     14.2     1899.9    3.752 DAY   AOS 3m 54s      43013    58.4402   -88.1201      779.6      0
---END_FRAME---
```

Columns: `NAME`, azimuth `AZ` (deg), elevation `EL` (deg), `RANGE` (km),
range-rate `RR(km/s)`, visibility `VIS` (VIS/DAY/ECL/HOR), `NEXT EVENT`
(AOS/LOS countdown), `NORAD` catalog number, sub-satellite `LAT`/`LON` (deg),
`APOGEE` (km), and `FLARE` (0 = none, >0 = specular flare). The text mirror
(port 12345) carries the same frame wrapped in HTML.

**Integration Notes:**
* Connect via TCP to receive continuous updates
* Parse frames by splitting on `---END_FRAME---`
* Data format matches terminal display (fixed-width columns)
* Disconnecting stops data transmission (no buffer accumulation)

**Firewall Configuration** (adjust if using `--port`):
```bash
sudo ufw allow 8080
sudo ufw allow 12345
sudo ufw allow 12346
```

---

## Celestrak Groups

Common group names for `--groupsel` or `group_selection`:

| Group | Description |
|:------|:------------|
| `active` | All active satellites (~6000+) |
| `stations` | Space stations (ISS, CSS, etc.) |
| `visual` | Bright/easily visible satellites |
| `amateur` | Amateur radio satellites |
| `weather` | Weather satellites |
| `noaa` | NOAA satellites |
| `goes` | GOES weather satellites |
| `starlink` | SpaceX Starlink constellation |
| `oneweb` | OneWeb constellation |
| `iridium` | Iridium (original) |
| `iridium-NEXT` | Iridium NEXT constellation |
| `gps-ops` | GPS satellites |
| `galileo` | Galileo navigation |
| `geo` | Geostationary satellites |

Multiple groups can be combined: `--groupsel amateur,weather,stations`

---

## Platform Notes

### Ubuntu Linux (PC/Laptop)
* **Performance:** High. Capable of tracking 20,000+ objects without UI lag.
* **Compiler:** Builds with Clang or GCC (C++17). CMake auto-selects Clang when it is installed and falls back to the system default (GCC) otherwise; override with `-DCMAKE_CXX_COMPILER=...`.
* **Radio Control:** Typically uses USB interfaces. Ensure your user is in the `dialout` group: `sudo usermod -aG dialout $USER`

### Raspberry Pi 5
* **Performance:** Optimized. Maintains <5% CPU utilization for ~13,000 objects.
* **Radio Control:** Can use USB or GPIO (HATs) for rig/rotor control.
* **Thermal:** Passive cooling sufficient for typical loads; active cooling recommended for 24/7 operation.

---

## Testing

### Functional Equivalence Test (Python vs C++)

A comprehensive equivalence test verifies that the Python (Skyfield) and C++ (libsgp4) implementations produce matching orbital calculations. The test compiles a minimal C++ test harness, runs both engines against the same TLE data, observer location, and fixed UTC time, then compares results.

**Running the test:**
```bash
# From project root — use whichever venv has skyfield/sgp4/numpy installed.
# The repo-root venv (created by `python3 -m venv venv && pip install -r
# python_tracker/requirements.txt`) works out of the box:
venv/bin/python tests/test_equivalence.py
```
The harness auto-discovers `libsgp4` via the same search order as CMake
(`~/sgp4/build/install` → `/usr/local` → `/usr`) and rebuilds the C++ test
tool on every run.

**What it tests:**
| Metric | Tolerance | Typical Difference |
|:-------|:----------|:-------------------|
| Azimuth / Elevation | 0.15 deg | < 0.001 deg |
| Slant Range | 2.0 km | < 0.03 km |
| Sub-satellite Lat/Lon | 0.05 deg | < 0.001 deg |
| Satellite Altitude | 1.0 km | < 0.002 km |
| Apogee | 1.0 km | < 0.001 km |
| Sun Position | 0.5 deg | < 0.005 deg |
| Visibility State | string match | see note below |

**Test satellites:** ISS (LEO, ~420 km), NOAA 19 (polar, ~860 km), GPS BIIR-2 (MEO, ~20,200 km).

**Visibility model note:** Both implementations use the same naked-eye visibility definition (above the horizon + observer Sun ≤ −12° + satellite sunlit). They differ only in the Sun model used to compute the observer's solar altitude — Python uses the JPL DE421 ephemeris, C++ uses a Meeus analytical model. These agree to within a few thousandths of a degree, so the two can disagree only for a satellite whose observer-Sun altitude sits within ~0.005° of the −12° boundary. The satellite illumination (Earth-shadow) calculation agrees between both implementations.

**Additional unit tests (Python, run by the equivalence test):**
- Apogee computation from TLE orbital elements (Kepler's third law)
- Decay detection (80 km apogee threshold)

### C++ Flare-Detection Unit Test

A standalone test of the specular-flare reflection geometry (`unittests/test_flare.cpp`):
```bash
# From project root
clang++ -std=c++17 -Iinclude unittests/test_flare.cpp src/visibility.cpp -o /tmp/test_flare && /tmp/test_flare
```
It covers a direct nadir flare (hit), a near-miss, an off-axis miss, high-orbit rejection (non-LEO), and daylight rejection. Expected output ends with `ALL TESTS PASSED`.

---

## License & Credits

* **Author**: Dr. Robert W. McGwier, PhD (N4HY)
* **AI Assistance**: Claude (Anthropic) for implementation
* **Based on**: *Quiktrak* (1981 VBasic, 1983 Commodore C, IBM C 1986, 1990, 1999)
* **License**: MIT — see [LICENSE](LICENSE)

ENJOYMENT IS REQUIRED. REPORT BUGS!
