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
#include "text_server.hpp"
#include "config_manager.hpp"
#include "pass_predictor.hpp"
#include "thread_pool.hpp"
#include "logger.hpp"
#include "rotator.hpp"
#include "rig_control.hpp"
#include "frequency_manager.hpp"

using namespace ve;

void print_help() {
    std::cout << "Usage: ./VisibleEphemeris [OPTIONS]\n\n"
              << "Options:\n"
              << "  --help, -h       Show help\n"
              << "  --lat <deg>      Override Latitude\n"
              << "  --lon <deg>      Override Longitude\n"
              << "  --alt <km>       Override Altitude\n"
              << "  --max_sats <N>   Override Max Satellites\n"
              << "  --maxapo <km>    Filter satellites by max apogee\n"
              << "  --minel <deg>    Filter satellites by minimum elevation\n"
              << "  --trail_mins <N> Override Trail Length (+/- minutes)\n"
              << "  --refresh        Force fresh TLE\n"
              << "  --groupsel <list> Comma-separated groups (e.g. \"amateur,weather,stations\")\n"
              << "  --satsel <list>   Comma-separated Satellite Names (Overrules groupsel)\n"
              << "  --no-visible     Radio Mode: Show all sats > min_el (ignore light)\n"
              << "  --all            Same as --no-visible\n"
              << "  --rigctl <en,port,host>        Configure Rig Control (e.g. T,4532,localhost)\n"
              << "  --rotctl <en,port,host,min_el> Configure Rotator Control (e.g. T,4533,localhost,0.0)\n"
              << "  --groupbuild     Launch Builder Mode (Web UI)\n"
              << "\nConfiguration is loaded from config.yaml by default.\n";
}

struct SharedState {
    std::mutex mutex;
    std::vector<DisplayRow> rows;
    std::vector<Satellite*> active_sats;
    bool updated = false;
};

// Helper to check string containment case-insensitive
bool hasString(const std::string& haystack, const std::string& needle) {
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char ch1, char ch2) { return std::toupper(ch1) == std::toupper(ch2); }
    );
    return (it != haystack.end());
}

std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    
    // 1. IMMEDIATE HELP CHECK
    for(int i=1; i<argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        }
    }
    
    Logger::log("Application Starting...");
    
    ConfigManager config_mgr("config.yaml");
    AppConfig config = config_mgr.load();
    if (config.lat == 0.0 && config.lon == 0.0) { config.lat = 39.5478; config.lon = -76.0916; }

    bool refresh_tle = false;
    bool builder_mode = false;

    // 2. Parse Arguments
    for(int i=1; i<argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--groupbuild") builder_mode = true;
        else if (arg == "--refresh") refresh_tle = true;
        else if (arg == "--lat") { if (i+1 < argc) config.lat = std::stod(argv[++i]); }
        else if (arg == "--lon") { if (i+1 < argc) config.lon = std::stod(argv[++i]); }
        else if (arg == "--alt") { if (i+1 < argc) config.alt = std::stod(argv[++i]); }
        else if (arg == "--max_sats") { if (i+1 < argc) config.max_sats = std::stoi(argv[++i]); }
        else if (arg == "--trail_mins") { if (i+1 < argc) config.trail_length_mins = std::stoi(argv[++i]); }
        else if (arg == "--maxapo") { if (i+1 < argc) config.max_apo = std::stod(argv[++i]); }
        else if (arg == "--minel") { if (i+1 < argc) config.min_el = std::stod(argv[++i]); }
        else if (arg == "--all") { config.show_all_visible = true; }
        else if (arg == "--groupsel") { if (i+1 < argc) config.group_selection = argv[++i]; config.sat_selection = ""; } 
        else if (arg == "--satsel") { if (i+1 < argc) config.sat_selection = argv[++i]; } 
        else if (arg == "--no-visible") { config.show_all_visible = true; } 
        else if (arg == "--rigctl") {
            if (i+1 < argc) {
                auto tokens = split(argv[++i], ',');
                if (tokens.size() >= 3) {
                    config.rig_enabled = (tokens[0] == "T" || tokens[0] == "true");
                    try { config.rig_port = std::stoi(tokens[1]); } catch(...) {}
                    config.rig_host = tokens[2];
                }
            }
        }
        else if (arg == "--rotctl") {
            if (i+1 < argc) {
                auto tokens = split(argv[++i], ',');
                if (tokens.size() >= 3) {
                    config.rotator_enabled = (tokens[0] == "T" || tokens[0] == "true");
                    try { config.rotator_port = std::stoi(tokens[1]); } catch(...) {}
                    config.rotator_host = tokens[2];
                    if (tokens.size() >= 4) {
                        try { config.rotator_min_el = std::stod(tokens[3]); } catch(...) {}
                    }
                }
            }
        }
    }

    // 3. AUTO-FIX CONFIG: If asking for GPS/GEO/GNSS or Specific Sats, disable Max Apo filter
    if (!config.sat_selection.empty() || 
        hasString(config.group_selection, "gps") || 
        hasString(config.group_selection, "gnss") || 
        hasString(config.group_selection, "geo")) {
        
        if (config.max_apo > 0 && config.max_apo < 20000) {
            std::cout << "[AUTO-FIX] Disabling Max Apogee filter (" << config.max_apo << "km) for High-Orbit targets.\n";
            Logger::log("Auto-disabled Max Apogee filter");
            config.max_apo = -1;
        }
    }

    bool track_sun = false;
    bool track_moon = false;

    // Check if Sun/Moon are requested via selection
    if (!config.sat_selection.empty()) {
        if (hasString(config.sat_selection, "sun")) track_sun = true;
        if (hasString(config.sat_selection, "moon")) track_moon = true;
    }

    // Check if Sun/Moon are requested via group "sunmoon"
    if (hasString(config.group_selection, "sunmoon")) {
        track_sun = true;
        track_moon = true;
    }
    
    try {
        std::cout << "Initializing TLE Manager..." << std::endl;
        TLEManager tle_mgr("./tle_cache");
        if(refresh_tle) tle_mgr.clearCache();

        // --- PHASE 1: BUILDER MODE ---
        if (builder_mode) {
            std::cout << "Starting Mission Planner UI on port 8080..." << std::endl;
            WebServer builder_server(8080, tle_mgr, true);
            builder_server.runBlocking(); 
            std::cout << "Configuration saved. Launching Tracker..." << std::endl;
            config = config_mgr.load(); 
        }
        
        // --- PHASE 2: TRACKER MODE ---
        std::cout << "Loading TLEs..." << std::endl;
        
        std::vector<Satellite> sats;
        if (!config.sat_selection.empty()) {
             std::cout << "Loading specific satellites: " << config.sat_selection << "..." << std::endl;
             sats = tle_mgr.loadSpecificSats(config.sat_selection);
        } else {
             std::cout << "Loading TLE groups: " << config.group_selection << "..." << std::endl;
             sats = tle_mgr.loadGroups(config.group_selection);
        }
        
        if (sats.empty() && !track_sun && !track_moon) {
            std::cerr << "ERROR: No satellites loaded! Check network or groups." << std::endl; 
            Logger::log("ERROR: No satellites loaded");
            return 1; 
        }
        Logger::log("Loaded " + std::to_string(sats.size()) + " satellites");

        WebServer web_server(8080, tle_mgr, false); 
        TextServer text_server(12345);
        
        Observer observer(config.lat, config.lon, config.alt);
        Display display; 
        display.setBlocking(true); 
        
        ThreadPool pool(4); 
        PassPredictor predictor(observer);
        
        std::unique_ptr<Rotator> rotator;
        if (config.rotator_enabled) {
            rotator = std::make_unique<Rotator>(config.rotator_host, config.rotator_port);
        }

        std::unique_ptr<RigControl> rig_ctl;
        std::unique_ptr<FrequencyManager> freq_mgr;
        if (config.rig_enabled) {
            std::cout << "[RIG] Enabling Rig Control (" << config.rig_host << ":" << config.rig_port << ")" << std::endl;
            rig_ctl = std::make_unique<RigControl>(config.rig_host, config.rig_port);
            std::cout << "[RIG] Initializing Frequency Manager..." << std::endl;
            freq_mgr = std::make_unique<FrequencyManager>("satnogs.json");
            if(freq_mgr->updateDatabase()) {
                std::cout << "[RIG] SatNOGS Database updated successfully." << std::endl;
            } else {
                std::cout << "[RIG] Using cached SatNOGS Database." << std::endl;
            }
        }
        
        web_server.start();
        text_server.start();

        auto last_calc_time = Clock::now();
        bool first_run = true;
        SharedState state;
        std::atomic<bool> running(true);

        // BACKGROUND MATH THREAD
        std::thread math_thread([&]() {
            while(running) {
                // HOT RELOAD CHECK
                if (web_server.hasPendingConfig()) {
                    AppConfig new_cfg = web_server.popPendingConfig();
                    if (new_cfg.group_selection != config.group_selection) {
                         Logger::log("Hot Reload: Switching groups to " + new_cfg.group_selection);
                         sats = tle_mgr.loadGroups(new_cfg.group_selection);
                    }
                    config = new_cfg;
                    observer = Observer(config.lat, config.lon, config.alt); 
                }

                auto now = Clock::now();
                std::vector<DisplayRow> local_rows;
                std::vector<Satellite*> local_sats;
                
                int rejected_apo = 0;
                int rejected_el = 0;
                int rejected_vis = 0;

                int selected_norad_id = web_server.getSelectedNoradId();

                // --- SUN & MOON ---
                {
                    // SUN (ID: -1)
                    {
                        Vector3 sun_eci = VisibilityCalculator::getSunPositionECI(now);
                        auto sun_look = observer.calculateLookAngle(sun_eci, now);
                        Geodetic sun_geo = VisibilityCalculator::getSunPositionGeo(now);
                        // Sun is always "Visible" (or Daylight) unless eclipsed (Eclipse of sun?) - Simplification: State is VISIBLE if el > 0
                        VisibilityCalculator::State sun_state = (sun_look.elevation > 0) ? VisibilityCalculator::State::DAYLIGHT : VisibilityCalculator::State::VISIBLE;
                        local_rows.push_back({"SUN", sun_look.azimuth, sun_look.elevation, sun_look.range, 0.0, sun_geo.lat_deg, sun_geo.lon_deg, 0.0, sun_state, -1, ""});
                    }

                    // MOON (ID: -2)
                    {
                        Vector3 moon_eci = VisibilityCalculator::getMoonPositionECI(now);
                        auto moon_look = observer.calculateLookAngle(moon_eci, now);
                        Geodetic moon_geo = VisibilityCalculator::getMoonPositionGeo(now);
                        // Moon visibility state
                        VisibilityCalculator::State moon_state = VisibilityCalculator::calculateState(moon_eci, observer.getPositionECI(now), now, moon_look.elevation);
                        local_rows.push_back({"MOON", moon_look.azimuth, moon_look.elevation, moon_look.range, 0.0, moon_geo.lat_deg, moon_geo.lon_deg, 0.0, moon_state, -2, ""});
                    }
                }

                for(auto& sat : sats) {
                    if(!running) break;
                    
                    // FILTER 1: APOGEE
                    if (config.max_apo > 0 && sat.getApogeeKm() > config.max_apo) {
                        rejected_apo++;
                        continue;
                    }

                    auto [pos, vel] = sat.propagate(now);
                    auto look = observer.calculateLookAngle(pos, now);
                    double rrate = observer.calculateRangeRate(pos, vel, now);

                    // ROTATOR LOGIC
                    if (rotator && rotator->isConnected() && sat.getNoradId() == selected_norad_id) {
                        if (look.elevation >= config.rotator_min_el) {
                            rotator->setPosition(look.azimuth, look.elevation);
                        }
                    }

                    // RIG CONTROL LOGIC (Doppler)
                    if (rig_ctl && rig_ctl->isConnected() && freq_mgr && sat.getNoradId() == selected_norad_id) {
                        if (freq_mgr->hasTransmitter(sat.getNoradId())) {
                            auto tx = freq_mgr->getBestTransmitter(sat.getNoradId());

                            // Calculate Doppler
                            // Range Rate is in km/s. Positive = Moving away.
                            // Downlink (Sat -> Earth): f_rx = f_dl * (1 - v/c)
                            // Uplink (Earth -> Sat): We need to transmit f_tx such that sat receives f_ul.
                            // Sat receives f_ul = f_tx * (1 - v/c). So f_tx = f_ul / (1 - v/c).

                            double c = 299792.458; // km/s
                            double factor = 1.0 - (rrate / c);

                            double dl_doppler = static_cast<double>(tx.downlink_low) * factor;
                            double ul_doppler = (tx.uplink_low > 0) ? static_cast<double>(tx.uplink_low) / factor : 0.0;

                            rig_ctl->setFrequencies(ul_doppler, dl_doppler);
                            rig_ctl->setMode(tx.mode);
                        }
                    }

                    if (config.show_all_visible) {
                        // Radio Mode: Show all
                    } else {
                        // Optical Mode:
                        if (look.elevation < config.min_el) { rejected_el++; continue; }
                        auto state = VisibilityCalculator::calculateState(pos, observer.getPositionECI(now), now, look.elevation);
                        if (state != VisibilityCalculator::State::VISIBLE) { rejected_vis++; continue; }
                    }

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
                    local_rows.push_back({sat.getName(), look.azimuth, look.elevation, look.range, rrate, geo.lat_deg, geo.lon_deg, sat.getApogeeKm(), state, sat.getNoradId(), next_event_str});
                    local_sats.push_back(&sat);
                }
                
                // DIAGNOSTICS: If empty list, report why
                if(local_rows.empty() && !sats.empty()) {
                     // Only log periodically to avoid spam, or handle in UI
                     // For now, we rely on console output from main
                }
                
                if (!running) break;

                std::sort(local_rows.begin(), local_rows.end(), [](const DisplayRow& a, const DisplayRow& b) { return a.el > b.el; });
                
                // Keep Sun/Moon even if filtered by resize
                std::vector<DisplayRow> preserved_bodies;
                for(const auto& r : local_rows) {
                    if (r.norad_id == -1 || r.norad_id == -2) preserved_bodies.push_back(r);
                }

                if (!config.show_all_visible) {
                    if (local_rows.size() > (size_t)config.max_sats) local_rows.resize(config.max_sats);
                } else {
                    if (local_rows.size() > 5000) local_rows.resize(5000);
                }

                // Re-inject if lost
                for(const auto& p : preserved_bodies) {
                    bool found = false;
                    for(const auto& r : local_rows) { if (r.norad_id == p.norad_id) { found = true; break; } }
                    if (!found) local_rows.push_back(p);
                }
                // Re-sort after re-injection to ensure correct order
                std::sort(local_rows.begin(), local_rows.end(), [](const DisplayRow& a, const DisplayRow& b) { return a.el > b.el; });

                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.rows = local_rows;
                    state.active_sats = local_sats;
                    state.updated = true;
                }
                
                for(int i=0; i<20; ++i) { if(!running) break; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
            }
        });

        // MAIN UI LOOP
        while (true) {
            display.setBlocking(true);
            auto input_res = display.handleInput();
            if (input_res == Display::InputResult::SAVE_AND_QUIT) { config_mgr.save(config); running=false; break; }
            else if (input_res == Display::InputResult::QUIT_NO_SAVE) { running=false; break; }

            std::vector<DisplayRow> current_rows;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.updated) {
                    current_rows = state.rows;
                    web_server.updateData(state.rows, state.active_sats, config);
                } else {
                    current_rows = state.rows; 
                }
            }
            
            display.update(current_rows, observer, Clock::now(), sats.size(), current_rows.size(), config.show_all_visible, config.min_el);
            text_server.updateData(display.getLastFrame()); 
        }

        web_server.stop();
        text_server.stop();
        if(math_thread.joinable()) math_thread.join();
        Logger::log("Shutdown Complete");

    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
