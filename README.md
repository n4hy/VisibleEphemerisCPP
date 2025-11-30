# Visible Ephemeris
### High-Performance Satellite Tracking Appliance (C++17)

**Visible Ephemeris** is a modern, spiritual successor to *Quiktrak* (1986), re-engineered for the Raspberry Pi 5 and modern silicon. It is capable of propagating 13,000+ satellites in real-time with sub-second updates while maintaining <5% CPU utilization.

It features a **Hybrid Decoupled Architecture** where the UI, Orbital Mechanics, and Network Services run on independent threads, ensuring the interface never freezes—even during heavy calculation loads.

---

## 🚀 Core Features

### 🛰️ Tracking Engine
* **SGP4/SDP4 Propagation**: Uses `libsgp4` for high-precision orbital math.
* **Massive Scale**: Tracks the entire NORAD Active Catalog (13,000+ objects) simultaneously.
* **Smart Caching**: Automatic TLE downloading and caching from Celestrak.
* **Multi-Group Selection**: Track specific combinations (e.g., `amateur,weather,stations`) using the `--groupsel` argument.

### 🖥️ Display Systems
* **NCurses Terminal Dashboard**: 
    * Flicker-free, color-coded real-time data table.
    * **Horizon Flash**: Visual indicator (Red/White flashing) when a satellite is rising or setting (within 1° of horizon).
* **Web Dashboard (Port 8080)**:
    * **Mercator Map**: Live ground tracks, "Red House" observer location, and auto-zoom to active horizon.
    * **Polar Skyplot**: Radar view of visible satellites with pulsing selection aura.
    * **Smart Trails**: Displays ground track history and future path (+/- N minutes).
* **Web Terminal Mirror (Port 12345)**:
    * Ultra-lightweight HTML mirror of the terminal output.
    * Uses HTTP/1.0 "Fire-and-Forget" protocol to prevent browser hanging on slow connections.

### 📻 Radio & Visual Modes
* **Optical Mode (Default)**: Filters satellites based on solar illumination (Sunlit satellite + Dark observer).
* **Radio Mode (`--no-visible`)**: Tracks all satellites above the horizon regardless of lighting conditions (Day/Night/Eclipse).
* **Doppler & Range Rate**: Real-time calculation of relative velocity (km/s) for radio tuning.

---

## 🛠️ Installation

### 1. Dependencies
Visible Ephemeris requires a C++17 compiler, CMake, and the following libraries:

**On Raspberry Pi / Debian:**
```bash
sudo apt update
sudo apt install build-essential cmake libncurses-dev libcurl4-openssl-dev libhamlib-dev
```

### 2. Install libsgp4 (Required)
This library provides the SGP4/SDP4 orbital mathematics. You must install it from source:

```bash
cd ~
git clone [https://github.com/dnwrnr/sgp4.git](https://github.com/dnwrnr/sgp4.git)
cd sgp4
mkdir build && cd build
cmake ..
make
sudo make install
sudo ldconfig
```

### 3. Build Visible Ephemeris

```bash
cd ~/VisibleEphemeris
mkdir build && cd build
cmake ..
make -j4
```

---

## 🎮 Usage
Run the application from the build directory. It will automatically generate a `config.yaml` and `tle_cache` directory on first run.

**Basic Command**
```bash
./VisibleEphemeris
```

### Common Examples

**1. Amateur Radio Mode (Ham Radio Satellites)**
Show all amateur satellites above the horizon, regardless of daylight.
```bash
./VisibleEphemeris --groupsel amateur --no-visible --minel 0
```

**2. Visual Observing (ISS & Bright Objects)**
Show only satellites that are sunlit while the observer is in darkness.
```bash
./VisibleEphemeris --groupsel "stations,visual" --minel 10
```

**3. Specific Location Override**
```bash
./VisibleEphemeris --lat 39.54 --lon -76.09 --alt 0.1
```

---

## ⚙️ Configuration & Arguments
The application saves your settings to `config.yaml` on exit. You can override these settings via command line arguments:

| Argument | Description | Default |
| :--- | :--- | :--- |
| `--lat <deg>` | Observer Latitude (Decimal Degrees) | 0.0 |
| `--lon <deg>` | Observer Longitude (Decimal Degrees) | 0.0 |
| `--alt <km>` | Observer Altitude (km) | 0.0 |
| `--groupsel <list>` | Comma-separated Celestrak groups (e.g., `amateur,weather`) | `active` |
| `--max_sats <N>` | Max number of satellites to display in the table | 100 |
| `--minel <deg>` | Minimum elevation filter | 0.0 |
| `--maxapo <km>` | Filter satellites with apogee > N km (e.g. 1000 for LEO) | -1 (Disabled) |
| `--no-visible` | Radio Mode: Ignore optical visibility constraints | False |
| `--trail_mins <N>` | Length of ground track trail (+/- minutes) | 5 |
| `--refresh` | Force fresh download of TLE data | False |

---

## ⌨️ Controls
The terminal interface is interactive:

* **Q**: Quit the application (Auto-saves configuration).
* **UP / DOWN**: Scroll the satellite list.
* **PAGE UP / PAGE DOWN**: Fast scroll.

---

## 🌐 Network Services
Visible Ephemeris acts as a web appliance, exposing two ports:

**1. Graphical Dashboard: `http://<IP>:8080`**
* Full interactive map and skyplot.
* Click table headers to sort by Name, Azimuth, Elevation, etc.
* Click a satellite to highlight it (pulsing aura) and see details.

**2. Text Mirror: `http://<IP>:12345`**
* Ultra-lightweight HTML reflection of the terminal screen.
* Uses HTTP/1.0 "Fire-and-Forget" protocol for maximum robustness on slow networks.

*Note: If you cannot access these ports, check your firewall:*
```bash
sudo ufw allow 8080
sudo ufw allow 12345
```

---

## 📜 License & Credits
* **Author**: Dr. Robert W. McGwier, PhD (N4HY)
* **Implementation Partner**: Gemini (AI) for WEB UI
* **Based on**: *Quiktrak* (1981 VBasic, 1983 Commodore C, IBM C 1986,1990,1999)
* **License**: MIT (or as specified by Author)
