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
#include <iostream>
#include <vector>
#include <thread>
#include <algorithm>
#include <atomic>
#include <csignal>

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
        else if (arg == "--no-visible") { config.show_all_visible = true; } // NEW FLAG
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
        
        Display display; 
        display.setBlocking(true); 
        
        ThreadPool pool(4); 
        PassPredictor predictor(observer);
        
        web_server.start();
        text_server.start();

        auto last_calc_time = Clock::now();
        bool first_run = true;
        std::vector<DisplayRow> final_list;

        Logger::log("Main Loop Starting");

        while (true) {
            display.setBlocking(true);
            auto input_res = display.handleInput();
            
            if (input_res == Display::InputResult::SAVE_AND_QUIT) { config_mgr.save(config); break; }
            else if (input_res == Display::InputResult::QUIT_NO_SAVE) { break; }

            auto now = Clock::now();
            if (first_run || std::chrono::duration_cast<std::chrono::milliseconds>(now - last_calc_time).count() > 1000) {
                last_calc_time = now;
                first_run = false;
                
                final_list.clear();
                std::vector<Satellite*> active_sats;

                display.setBlocking(false);
                int sat_count = 0;
                bool break_inner = false;

                for(auto& sat : sats) {
                    if (break_inner) break;

                    sat_count++;
                    if (sat_count % 50 == 0) {
                        auto ir = display.handleInput();
                        if (ir == Display::InputResult::BREAK_LOOP) {
                            break_inner = true; 
                        }
                    }

                    if (config.max_apo > 0 && sat.getApogeeKm() > config.max_apo) continue;

                    auto [pos, vel] = sat.propagate(now);
                    auto look = observer.calculateLookAngle(pos, now);
                    double rrate = observer.calculateRangeRate(pos, vel, now);

                    // NEW LOGIC: If show_all_visible (--no-visible) is true, we BYPASS Elevation Filter
                    if (config.show_all_visible) {
                        // NO FILTER: Add everything, even negative EL
                    } else {
                        // OPTICAL MODE: Must meet El AND be optically visible
                        if (look.elevation < config.min_el) continue;
                        auto state = VisibilityCalculator::calculateState(pos, observer.getPositionECI(now), now, look.elevation);
                        if (state != VisibilityCalculator::State::VISIBLE) continue;
                    }

                    // Recalculate state for display row
                    auto state = VisibilityCalculator::calculateState(pos, observer.getPositionECI(now), now, look.elevation);

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
                        final_list.push_back({sat.getName(), look.azimuth, look.elevation, look.range, rrate, geo.lat_deg, geo.lon_deg, sat.getApogeeKm(), state, sat.getNoradId(), next_event_str});
                        active_sats.push_back(&sat);
                }
                
                if (!break_inner) {
                    std::sort(final_list.begin(), final_list.end(), [](const DisplayRow& a, const DisplayRow& b) { return a.el > b.el; });
                    if (final_list.size() > (size_t)config.max_sats) final_list.resize(config.max_sats);
                    web_server.updateData(final_list, active_sats, config); 
                }
            }
            display.update(final_list, observer, now, sats.size(), final_list.size(), config.show_all_visible, config.min_el);
            text_server.updateData(display.getLastFrame()); 
        }

        web_server.stop();
        text_server.stop();
        Logger::log("Shutdown Complete");

    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
