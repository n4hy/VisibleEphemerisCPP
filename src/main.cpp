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
              << "  --satsel <list>   Comma-separated Satellite Names (Overrules groupsel)\n"
              << "  --no-visible     Radio Mode: Show all sats > min_el (ignore light)\n"
              << "  --time <str>     Simulate time (e.g. \"2025-01-01 12:00:00\")\n"
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
    std::chrono::seconds time_offset(0);
    bool sim_time = false;

    // 2. Parse Arguments
    for(int i=1; i<argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--groupbuild") builder_mode = true;
        else if (arg == "--refresh") refresh_tle = true;
        else if (arg == "--time") {
            if (i+1 < argc) {
                std::string t_str = argv[++i];
                std::tm t = {};
                std::stringstream ss(t_str);
                ss >> std::get_time(&t, "%Y-%m-%d %H:%M:%S");
                if (ss.fail()) {
                    std::cerr << "Invalid time format. Use \"YYYY-MM-DD HH:MM:SS\"" << std::endl;
                    return 1;
                }
                // Interpret as Local Time (mktime)
                t.tm_isdst = -1; // Let mktime determine DST
                std::tm t_input_copy = t; // Keep copy for offset calc

                std::time_t utc_timestamp = std::mktime(&t); // Converts Local -> UTC Timestamp
                auto sim_tp = std::chrono::system_clock::from_time_t(utc_timestamp);

                // Calculate Manual Offset: (Simulated UTC - What it would be if Input was UTC)
                // Actually, we want: Input Time - Displayed UTC Time
                // Better: We calculated 'utc_timestamp'. If we display 'utc_timestamp' via gmtime, we get the UTC representation.
                // We want to display the equivalent of 't_input_copy'.
                // So we need an offset that, when added to 'utc_timestamp', yields a time_t that gmtime prints as 't_input_copy'.

                // Let's assume input was UTC to get its "face value" as time_t
                // Portable timegm replacement since we can't rely on timegm existing
                std::tm tm_utc = t_input_copy;
                // timegm is not standard C++, but we can use a trick or valid C logic if available.
                // Or simplified: manual_offset = mktime(t) - mktime(gmtime(mktime(t))) ??? No.

                // Safe approach:
                // 1. We have 'utc_timestamp' (The actual point in time).
                // 2. gmtime(&utc_timestamp) gives us the broken down UTC.
                // 3. We want the display to match 't_input_copy'.
                // 4. Determine difference in seconds between broken-down UTC and broken-down Input.

                std::tm* true_utc_parts = std::gmtime(&utc_timestamp);

                // Convert both to seconds from start of day to find difference (ignoring date wrap for a second, but need full diff)
                // Easier: Use mktime on both, treating them as local, to find the delta?
                // No, that brings timezone back in.

                // Let's rely on the fact that mktime adjusted by 'offset'.
                // offset = utc_timestamp - (timestamp if input was UTC).
                // But we don't have "timestamp if input was UTC" easily without timegm.

                // Let's use the raw difference between the resulting UTC struct and the Input struct.
                // Since we want Display = Input.
                // Display = GMTime(Timestamp + OFFSET).
                // GMTime(Timestamp) = true_utc_parts.
                // We want GMTime(Timestamp + OFFSET) = t_input_copy.
                // So OFFSET ~= t_input_copy - true_utc_parts.

                // We can approximate this by using mktime on both structs (treating both as local).
                // The difference in resulting time_t's will match the difference in the structs.
                std::time_t t_1 = std::mktime(&t_input_copy); // Already calculated as utc_timestamp, but re-do ensures consistency
                std::time_t t_2 = std::mktime(true_utc_parts);
                config.manual_time_offset = (long)std::difftime(t_1, t_2);

                time_offset = std::chrono::duration_cast<std::chrono::seconds>(sim_tp - Clock::now());
                sim_time = true;
                Logger::log("Simulating Time (Local): " + t_str + " (Offset: " + std::to_string(time_offset.count()) + "s, DispOffset: " + std::to_string(config.manual_time_offset) + "s)");
            }
        }
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
        
        if (sats.empty()) { 
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

                auto now = Clock::now() + time_offset;
                std::vector<DisplayRow> local_rows;
                std::vector<Satellite*> local_sats;
                
                int rejected_apo = 0;
                int rejected_el = 0;
                int rejected_vis = 0;

                int selected_norad_id = web_server.getSelectedNoradId();

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

                    if (config.show_all_visible) {
                        // Radio Mode: Show all
                    } else {
                        // Optical Mode:
                        if (look.elevation < config.min_el) { rejected_el++; continue; }
                        auto state = VisibilityCalculator::calculateState(pos, observer.getPositionECI(now), now, look.elevation);
                        if (state != VisibilityCalculator::State::VISIBLE) { rejected_vis++; continue; }
                    }

                    auto state = VisibilityCalculator::calculateState(pos, observer.getPositionECI(now), now, look.elevation);
                    int flare_status = 0;
                    if (state == VisibilityCalculator::State::VISIBLE) {
                        flare_status = VisibilityCalculator::checkFlare(pos, observer.getPositionECI(now), VisibilityCalculator::getSunPositionECI(now), sat.getApogeeKm());
                    }

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
                    local_rows.push_back({sat.getName(), look.azimuth, look.elevation, look.range, rrate, geo.lat_deg, geo.lon_deg, sat.getApogeeKm(), state, sat.getNoradId(), next_event_str, flare_status});
                    local_sats.push_back(&sat);
                }
                
                // DIAGNOSTICS: If empty list, report why
                if(local_rows.empty() && !sats.empty()) {
                     // Only log periodically to avoid spam, or handle in UI
                     // For now, we rely on console output from main
                }
                
                if (!running) break;

                std::sort(local_rows.begin(), local_rows.end(), [](const DisplayRow& a, const DisplayRow& b) { return a.el > b.el; });
                
                if (!config.show_all_visible) {
                    if (local_rows.size() > (size_t)config.max_sats) local_rows.resize(config.max_sats);
                } else {
                    if (local_rows.size() > 5000) local_rows.resize(5000);
                }

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
                    web_server.updateData(state.rows, state.active_sats, config, Clock::now() + time_offset);
                } else {
                    current_rows = state.rows; 
                }
            }
            
            display.update(current_rows, observer, Clock::now() + time_offset, sats.size(), current_rows.size(), config.show_all_visible, config.min_el);
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
