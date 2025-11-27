#include <iostream>
#include <vector>
#include <thread>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <sstream>
#include "satellite.hpp"
#include "observer.hpp"
#include "visibility.hpp"
#include "tle_manager.hpp"
#include "display.hpp"
#include "web_server.hpp"
#include "text_server.hpp"
#include "config_manager.hpp"
#include "pass_predictor.hpp"
#include "thread_pool.hpp"
#include "logger.hpp"

using namespace ve;

void print_help() {
    std::cout << "Usage: ./VisibleEphemeris [OPTIONS]\n\n"
              << "Options:\n"
              << "  --help, -h       Show help\n"
              << "  --lat <deg>      Override Latitude\n"
              << "  --lon <deg>      Override Longitude\n"
              << "  --max_sats <N>   Override Max Satellites\n"
              << "  --trail_mins <N> Override Trail Length (+/- minutes)\n"
              << "  --refresh        Force fresh TLE\n"
              << "  --groupsel <list> Comma-separated groups (e.g. \"amateur,weather,stations\")\n"
              << "  --no-visible     Radio Mode: Show all sats > min_el (ignore light)\n"
              << "\nConfiguration is loaded from config.yaml by default.\n";
}

// --- DECOUPLED ARCHITECTURE: SHARED STATE ---
struct SharedState {
    std::mutex mutex;
    std::vector<DisplayRow> rows;
    std::vector<Satellite*> active_sats;
    bool updated = false;
};

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    
    Logger::log("Application Starting...");
    
    ConfigManager config_mgr("config.yaml");
    AppConfig config = config_mgr.load();
    // Default to approximate user location if zero
    if (config.lat == 0.0 && config.lon == 0.0) { config.lat = 39.5478; config.lon = -76.0916; }

    bool refresh_tle = false;

    for(int i=1; i<argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { print_help(); return 0; }
        else if (arg == "--lat") { if (i+1 < argc) config.lat = std::stod(argv[++i]); }
        else if (arg == "--lon") { if (i+1 < argc) config.lon = std::stod(argv[++i]); }
        else if (arg == "--max_sats") { if (i+1 < argc) config.max_sats = std::stoi(argv[++i]); }
        else if (arg == "--trail_mins") { if (i+1 < argc) config.trail_length_mins = std::stoi(argv[++i]); }
        else if (arg == "--maxapo") { if (i+1 < argc) config.max_apo = std::stod(argv[++i]); }
        else if (arg == "--minel") { if (i+1 < argc) config.min_el = std::stod(argv[++i]); }
        else if (arg == "--all") { config.show_all_visible = true; }
        else if (arg == "--groupsel") { if (i+1 < argc) config.group_selection = argv[++i]; }
        else if (arg == "--no-visible") { config.show_all_visible = true; } 
        else if (arg == "--refresh") { refresh_tle = true; }
    }
    
    try {
        WebServer web_server(8080);
        TextServer text_server(12345);
        
        std::cout << "Loading TLEs for groups: " << config.group_selection << "..." << std::endl;
        Logger::log("Loading TLEs for groups: " + config.group_selection);
        
        TLEManager tle_mgr("./tle_cache");
        if(refresh_tle) tle_mgr.clearCache();
        
        std::vector<Satellite> sats = tle_mgr.loadGroups(config.group_selection);
        
        if (sats.empty()) { 
            std::cerr << "ERROR: No satellites loaded!" << std::endl; 
            Logger::log("ERROR: No satellites loaded");
            return 1; 
        }
        Logger::log("Loaded " + std::to_string(sats.size()) + " satellites");

        Observer observer(config.lat, config.lon, config.alt);
        
        // SHARED STATE FOR UI HANDOFF
        SharedState state;
        std::atomic<bool> running(true);

        // BACKGROUND MATH THREAD
        std::thread math_thread([&]() {
            ThreadPool pool(4); 
            PassPredictor predictor(observer);
            
            while(running) {
                auto now = Clock::now();
                // FIX: Variable names declared here must match usage below
                std::vector<DisplayRow> local_rows;
                std::vector<Satellite*> local_sats;

                for(auto& sat : sats) {
                    if(!running) break;
                    if (config.max_apo > 0 && sat.getApogeeKm() > config.max_apo) continue;

                    auto [pos, vel] = sat.propagate(now);
                    auto look = observer.calculateLookAngle(pos, now);
                    double rrate = observer.calculateRangeRate(pos, vel, now);

                    // LOGIC: Filter based on visibility settings
                    if (config.show_all_visible) {
                        // Radio mode: Add everything above horizon (handled by min_el usually, or just add)
                    } else {
                        // Optical Mode: Must meet El AND be optically visible
                        if (look.elevation < config.min_el) continue;
                        auto check_state = VisibilityCalculator::calculateState(pos, observer.getPositionECI(now), now, look.elevation);
                        if (check_state != VisibilityCalculator::State::VISIBLE) continue;
                    }

                    // Recalculate state for display row (redundant but safe)
                    auto state_val = VisibilityCalculator::calculateState(pos, observer.getPositionECI(now), now, look.elevation);

                    // Async Compute Pass/Trail if missing
                    bool needs_update = sat.getPredictedPasses().empty() || sat.getFullTrackCopy().empty();
                    if (needs_update && !sat.is_computing.exchange(true)) {
                        pool.enqueue([&sat, &predictor, now, config]() {
                            auto passes = predictor.predict(sat, now);
                            sat.setPredictedPasses(passes);
                            sat.calculateGroundTrack(now, config.trail_length_mins, 60);
                            sat.is_computing = false;
                        });
                    }
                    
                    std::string next_event_str = "Calculating...";
                    auto passes = sat.getPredictedPasses();
                    if (!passes.empty()) {
                             auto& next = passes[0];
                             long diff = std::chrono::duration_cast<std::chrono::seconds>(next.time - now).count();
                             if (diff < 0) { std::vector<Satellite::PassEvent> e; sat.setPredictedPasses(e); }
                             else {
                                 int mm = diff / 60; int ss = diff % 60;
                                 std::stringstream ts; ts << (next.is_aos ? "AOS " : "LOS ") << mm << "m " << ss << "s";
                                 next_event_str = ts.str();
                             }
                        }
                        
                        auto geo = sat.getGeodetic(now);
                        
                        // FIX: Ensure we use the variables declared at top of loop (local_rows, local_sats)
                        local_rows.push_back({sat.getName(), look.azimuth, look.elevation, look.range, rrate, geo.lat_deg, geo.lon_deg, sat.getApogeeKm(), state_val, sat.getNoradId(), next_event_str});
                        local_sats.push_back(&sat);
                }

                // Sort by Elevation (Descending)
                std::sort(local_rows.begin(), local_rows.end(), [](const DisplayRow& a, const DisplayRow& b) { return a.el > b.el; });
                
                // Crop to max_sats
                if (local_rows.size() > (size_t)config.max_sats) local_rows.resize(config.max_sats);

                // PUSH TO SHARED STATE
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.rows = local_rows;
                    state.active_sats = local_sats;
                    state.updated = true;
                }
                
                // Throttle Math Loop (50ms)
                for(int i=0; i<20; ++i) { if(!running) break; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
            }
        });

        // UI THREAD (MAIN)
        Display display; 
        web_server.start();
        text_server.start();
        Logger::log("UI Loop Started");

        while (true) {
            auto input = display.handleInput();
            if (input == Display::InputResult::SAVE_AND_QUIT) { config_mgr.save(config); running=false; break; }
            else if (input == Display::InputResult::QUIT_NO_SAVE) { running=false; break; }

            // Check for new data
            std::vector<DisplayRow> current_rows;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.updated) {
                    current_rows = state.rows;
                    web_server.updateData(state.rows, state.active_sats, config);
                } else {
                    current_rows = state.rows; // Use last known good frame
                }
            }
            
            display.update(current_rows, observer, Clock::now(), sats.size(), current_rows.size(), config.show_all_visible, config.min_el);
            text_server.updateData(display.getLastFrame());
            
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 20FPS UI
        }

        if(math_thread.joinable()) math_thread.join();
        web_server.stop();
        text_server.stop();
        Logger::log("Shutdown Complete");

    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
