# Visible Ephemeris
### High-Performance Satellite Tracking Appliance (C++17 & Python)

**Visible Ephemeris** is a modern, spiritual successor to *Quiktrak* (1986), re-engineered for the Raspberry Pi 5 and modern silicon. It is capable of propagating 13,000+ satellites in real-time with sub-second updates while maintaining <5% CPU utilization.

It features a **Hybrid Decoupled Architecture** where the UI, Orbital Mechanics, and Network Services run on independent threads, ensuring the interface never freezes—even during heavy calculation loads.

Both **C++** and **Python** implementations are provided with identical functionality.

---

## Core Features

### Tracking Engine
* **SGP4/SDP4 Propagation**: Uses `libsgp4` (C++) or `Skyfield` (Python) for high-precision orbital math.
* **Massive Scale**: Tracks the entire NORAD Active Catalog (13,000+ objects) simultaneously.
* **Smart Caching**: Automatic TLE downloading and caching from Celestrak with 24-hour auto-refresh cycle.
* **Multi-Group Selection**: Track specific combinations (e.g., `amateur,weather,stations`) using the `group_selection` config or `--groupsel` argument.
* **Stability**: Implements "Pre-calculate All" logic at startup to ensure 24-hour pass predictions are instantly available, eliminating "Calculating..." flicker and UI jitter.
* **Decoupled Clock**: Simulation time input is treated as "Face Value" (Local Wall-Clock Time) for display, while strictly adhering to UTC for orbital physics, eliminating timezone confusion.

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

---

## Operating Modes

### Radio Mode (`visible_only: false`)
Shows **ALL satellites** in the selected group(s), color-coded by elevation and visibility:

| Color | Condition | Description |
|:------|:----------|:------------|
| **Yellow** | Above min_el AND Visible | Satellite is sunlit, observer in darkness - optimal for visual observation |
| **Green** | Above min_el AND NOT Visible | Satellite above minimum elevation but in daylight or eclipsed - good for radio |
| **Grey** | Below min_el OR Below Horizon | Satellite is low or not yet risen - displayed for situational awareness |

This mode displays every satellite in the group on the map and in tables, limited only by `max_sats`.

### Optical Mode (`visible_only: true`)
Shows only satellites that are:
1. Above the minimum elevation (`min_el`)
2. Optically visible (sunlit satellite with observer in darkness)

This mode is optimized for visual observers who only want to see satellites they can actually spot.

### Hardware Control
* **Radio Control**: Automated Hamlib control for Transceiver Frequency/Mode (Doppler correction). *Requires single satellite selection.*
* **Rotator Control**: Automated Hamlib control for Azimuth/Elevation tracking. *Requires single satellite selection.*

---

## Installation

### C++ Version (Primary)

We provide an automated build script `build.sh` that handles dependencies (including building `libsgp4` from source) and installation.

```bash
cd VisibleEphemeris
chmod +x build.sh
./build.sh
```

**Note:** The script utilizes `sudo` to install dependencies and the final binary.

#### Manual Build
```bash
mkdir build && cd build
cmake ..
make -j4
```

### Python Version

The Python tracker is located in `python_tracker/` and provides identical functionality.

**Prerequisites:**
* Python 3.10+
* Linux Environment (required for `termios`/`tty` interactive input support)

**Installation:**
```bash
cd python_tracker

# Create virtual environment with system packages
python3 -m venv --system-site-packages venv
source venv/bin/activate

# Install dependencies
pip install -r requirements.txt
```

**Running:**
```bash
source venv/bin/activate  # If not already activated
python3 main.py
```

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
visible_only: false       # false = Radio Mode (all sats), true = Optical Mode
rotator_control: false    # Enable Hamlib rotator control
rotator_host: localhost   # Rotator daemon host
rotator_port: 4533        # Rotator daemon port
rotator_min_el: 0         # Minimum elevation for rotator tracking
```

### Command Line Arguments

| Argument | Description | Default |
|:---------|:------------|:--------|
| `--lat <deg>` | Observer Latitude (Decimal Degrees) | from config |
| `--lon <deg>` | Observer Longitude (Decimal Degrees) | from config |
| `--alt <km>` | Observer Altitude (km) | from config |
| `--groupsel <list>` | Comma-separated Celestrak groups (e.g., `amateur,weather`) | `active` |
| `--satsel <list>` | Comma-separated Satellite Names (overrides groupsel) | None |
| `--visible` | Optical Mode: show only sunlit satellites | from config |
| `--no-visible` | Radio Mode: show ALL satellites (Python only) | - |
| `--minel <deg>` | Minimum elevation filter | 0.0 |
| `--maxsats <N>` | Maximum satellites to display | 100 |
| `--maxapo <km>` | Filter satellites with apogee > N km | -1 (disabled) |
| `--trail_mins <N>` | Ground track trail length (+/- minutes) | 5 |
| `--rotator` | Enable Hamlib Rotator Control | false |
| `--refresh` | Force fresh download of TLE data | false |
| `--time <str>` | Simulate time (Format: "YYYY-MM-DD HH:MM:SS") | Real-time |

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

---

## Keyboard Controls

| Key | Action |
|:----|:-------|
| **Q** | Quit (prompts to save configuration) |
| **UP/DOWN** | Scroll satellite list |
| **PAGE UP/DOWN** | Fast scroll |

---

## Network Services

Visible Ephemeris exposes two web interfaces:

### Graphical Dashboard: `http://<IP>:8080`
* Interactive Mercator map with satellite positions and ground tracks
* Polar skyplot (toggle with MAP/SKY button)
* Sortable satellite table
* Click satellites to select and view details
* Solar terminator visualization
* Satellite footprint radius for selected satellite

### Text Mirror: `http://<IP>:12345`
* Lightweight HTML reflection of terminal output
* Auto-refreshes every second
* Works on low-bandwidth connections

**Firewall Configuration:**
```bash
sudo ufw allow 8080
sudo ufw allow 12345
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
* **Compiler:** Uses Clang by default on Ubuntu 24.04.
* **Radio Control:** Typically uses USB interfaces. Ensure your user is in the `dialout` group: `sudo usermod -aG dialout $USER`

### Raspberry Pi 5
* **Performance:** Optimized. Maintains <5% CPU utilization for ~13,000 objects.
* **Radio Control:** Can use USB or GPIO (HATs) for rig/rotor control.
* **Thermal:** Passive cooling sufficient for typical loads; active cooling recommended for 24/7 operation.

---

## License & Credits

* **Author**: Dr. Robert W. McGwier, PhD (N4HY)
* **AI Assistance**: Claude (Anthropic) for implementation
* **Based on**: *Quiktrak* (1981 VBasic, 1983 Commodore C, IBM C 1986, 1990, 1999)
* **License**: MIT
ENJOYMENT IS REQUIRED. REPORT BUGS!
