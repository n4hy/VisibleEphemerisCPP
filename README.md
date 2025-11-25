# **Visible Ephemeris C++**

**Visible Ephemeris** is a high-performance, real-time satellite tracking and pass prediction engine designed for the Raspberry Pi. Written in modern C++17, it calculates orbital mechanics for 13,000+ active satellites in real-time, providing a professional split-screen dashboard and a responsive web-based map interface.

*(Add a screenshot of your terminal/web interface here)*

## **Features**

* **High-Precision Physics:** Uses the SGP4/SDP4 propagation model (Vallado) with 2025 Epoch support.  
* **Real-Time Tracking:** Calculates positions for the entire NORAD active catalog (\~13,000 objects) every second.  
* **Solar Dynamics:** Accurate eclipse detection and visibility calculations based on observer location and solar position.  
* **Split-Screen Web Interface:**  
  * **Live Map:** Full-screen Mercator projection with real-time satellite markers.  
  * **Skyplot (Fisheye):** Polar plot showing satellites as they appear overhead.  
  * **Data Table:** Sortable, clickable list of visible satellites.  
* **Terminal Dashboard:** Professional NCurses-based TUI for headless operation.  
* **Smart Filtering:** Filter by visibility (sunlit only) or orbit type (LEO vs MEO/GEO).

## **Quick Start**

\# Run with default settings (Havre de Grace, MD)  
./VisibleEphemeris

\# Run with custom location (Lat, Lon, Max Satellites)  
./VisibleEphemeris 30.5 \-97.0 50

\# Show all satellites above horizon (including eclipsed)  
./VisibleEphemeris \--all

\# Filter out high-orbit objects (e.g., only LEO \< 2000km apogee)  
./VisibleEphemeris \--maxapo 2000

## **Architecture**

The system is built as a single-process, multi-threaded C++ application:

1. **TLE Manager:** Handles fetching, caching, and parsing NORAD Two-Line Element sets.  
2. **Propagator:** A multi-threaded engine that runs SGP4 calculations for thousands of objects in parallel.  
3. **Observer:** Calculates Topocentric coordinates (Azimuth/Elevation) and Solar lighting angles.  
4. **Web Server:** A custom, non-blocking HTTP server that serves the frontend and a JSON API.

## **Credits**

* **Author:** Dr. Robert W. McGwier, PhD (N4HY)  
* **Core Logic:** Based on the original "Quiktrak" (1986).  
* **Libraries:** libsgp4 (Vallado), ncurses, libcurl, Leaflet.js (Frontend).
