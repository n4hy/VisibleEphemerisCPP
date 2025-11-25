#include <iostream>
#include <vector>
#include <thread>
#include <algorithm>
#include <atomic>
#include <csignal>
#include "satellite.hpp"
#include "observer.hpp"
#include "visibility.hpp"
#include "tle_manager.hpp"
#include "display.hpp"
#include "web_server.hpp"

using namespace ve;

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    
    double lat = 39.5478; double lon = -76.0916; double alt = 0.0;
    int max_sats = 20;
    bool show_all_visible = false;
    double max_apogee = -1.0; // -1 means no filter

    // Parse Args
    for(int i=1; i<argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--all" || arg == "--visible=no") {
            show_all_visible = true;
        } 
        else if (arg == "--maxapo") {
            if (i+1 < argc) {
                try { max_apogee = std::stod(argv[++i]); } catch(...) {}
            }
        }
        else if (i == 1 && arg[0] != '-') {
            try { lat = std::stod(arg); } catch(...) {}
        } 
        else if (i == 2 && arg[0] != '-') {
            try { lon = std::stod(arg); } catch(...) {}
        } 
        else if (i == 3 && arg[0] != '-') {
            try { max_sats = std::stoi(arg); } catch(...) {}
        }
    }

    std::cout << "Loading TLEs... (Please wait)" << std::endl;
    TLEManager tle_mgr("./tle_cache");
    for(int i=0; i<argc; ++i) { if(std::string(argv[i]) == "--refresh") tle_mgr.clearCache(); }
    std::vector<Satellite> sats = tle_mgr.loadSatellites("https://celestrak.org/NORAD/elements/gp.php?GROUP=active&FORMAT=tle");

    if (sats.empty()) { std::cerr << "ERROR: No satellites loaded!" << std::endl; return 1; }

    Observer observer(lat, lon, alt);
    Display display; 
    WebServer web_server(8080);
    
    web_server.start();

    while (true) {
        if (display.shouldQuit()) break;

        TimePoint current_time = Clock::now();
        std::vector<DisplayRow> final_list;
        final_list.reserve(500);

        // DEBUG: Track filtering
        int apo_filtered = 0;

        for(const auto& sat : sats) {
            // Pre-filter by Apogee if set (Efficiency)
            double apogee = sat.getApogeeKm();
            if (max_apogee > 0 && apogee > max_apogee) {
                apo_filtered++;
                continue;
            }

            auto [pos, vel] = sat.propagate(current_time);
            auto look = observer.calculateLookAngle(pos, current_time);
            
            if (look.elevation > 0) {
                auto state = VisibilityCalculator::calculateState(pos, observer.getPositionECI(current_time), current_time, look.elevation);
                bool keep = show_all_visible ? true : (state == VisibilityCalculator::State::VISIBLE);

                if (keep) {
                    auto geo = sat.getGeodetic(current_time);
                    final_list.push_back({sat.getName(), look.azimuth, look.elevation, look.range, geo.lat_deg, geo.lon_deg, apogee, state, sat.getNoradId()});
                }
            }
        }
        
        // Print debug first few frames to ensure filter is working
        static int debug_frames = 0;
        if (debug_frames < 3 && max_apogee > 0) {
            // Use cerr to avoid messing up NCurses
             // std::cerr << "DEBUG: Apogee Filter removed " << apo_filtered << " satellites." << std::endl;
             debug_frames++;
        }

        std::sort(final_list.begin(), final_list.end(), [](const DisplayRow& a, const DisplayRow& b) {
            return a.el > b.el; 
        });

        if (final_list.size() > (size_t)max_sats) {
            final_list.resize(max_sats);
        }

        display.update(final_list, observer, current_time, sats.size(), max_sats, final_list.size());
        web_server.updateData(final_list); 
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    web_server.stop();
    return 0;
}
