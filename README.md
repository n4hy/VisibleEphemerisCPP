# Visible Ephemeris
### High-Performance Satellite Tracking Appliance (C++17)

**Visible Ephemeris** is a modern, spiritual successor to *Quiktrak* (1986), re-engineered for the Raspberry Pi 5 and modern silicon. It is capable of propagating 13,000+ satellites in real-time with sub-second updates while maintaining <5% CPU utilization.

It features a hybrid decoupled architecture where the UI, Orbital Mechanics, and Network Services run on independent threads, ensuring the interface never freezes—even during heavy calculation loads.

---

## 🚀 Core Features

### 🛰️ Tracking Engine
* **SGP4/SDP4 Propagation**: Uses `libsgp4` for high-precision orbital math.
* **Massive Scale**: Tracks the entire NORAD Active Catalog (13,000+ objects) simultaneously.
* **Smart Caching**: Automatic TLE downloading and caching from Celestrak.
* **Multi-Group Selection**: Track specific combinations (e.g., `amateur,weather,stations`).

### 🖥️ Display Systems
* **NCurses Terminal Dashboard**: 
    * Flicker-free, color-coded real-time data table.
    * **Horizon Flash**: Visual indicator (Red/White flashing) when a satellite is rising or setting (within 1° of horizon).
* **Web Dashboard (Port 8080)**:
    * **Mercator Map**: Live ground tracks, "Red House" observer location, and auto-zoom to active horizon.
    * **Polar Skyplot**: Radar view of visible satellites.
    * **Smart Trails**: Displays ground track history and future path (+/- N minutes).
* **Web Terminal Mirror (Port 12345)**:
    * Raw, low-bandwidth HTML mirror of the terminal output (ideal for remote monitoring).

### 📻 Radio & Visual Modes
* **Optical Mode (Default)**: Filters satellites based on solar illumination (Sunlit satellite + Dark observer).
* **Radio Mode (`--no-visible`)**: Tracks all satellites above the horizon regardless of lighting conditions.
* **Doppler & Range Rate**: Real-time calculation of relative velocity (km/s) for radio tuning.

---

## 🛠️ Installation

### 1. Dependencies
Visible Ephemeris requires a C++17 compiler, CMake, and the following libraries:
* **ncurses**: For the terminal UI.
* **libcurl**: For downloading TLEs.
* **libsgp4**: The core orbital mechanics library.

**On Raspberry Pi / Debian:**
```bash
sudo apt update
sudo apt install build-essential cmake libncurses-dev libcurl4-opensh

[Image of software architecture diagram]
ssl-dev




