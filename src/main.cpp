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
              << "  --visible <bool> Limit to Optically Visible only (true/false)\n"
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

// Portable timegm implementation (UTC mktime)
// Converts tm struct to time_t assuming UTC, ignoring tm_isdst
std::time_t timegm_portable(struct tm* tm) {
    if (!tm) return 0;

    // Days in each month (non-leap)
    static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    long year = tm->tm_year + 1900;
    int month = tm->tm_mon; // 0-11

    // Calculate total days from 1970
    long days = 0;

    // Add days for years
    for (long y = 1970; y < year; ++y) {
        bool is_leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += is_leap ? 366 : 365;
    }

    // Add days for months in current year
    bool current_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    for (int m = 0; m < month; ++m) {
        if (m == 1 && current_leap) days += 29;
        else days += days_in_month[m];
    }

    days += (tm->tm_mday - 1);

    std::time_t total_seconds = days * 86400;
    total_seconds += tm->tm_hour * 3600;
    total_seconds += tm->tm_min * 60;
    total_seconds += tm->tm_sec;

    return total_seconds;
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

    // DECOUPLED CLOCK VARIABLES
    std::time_t display_epoch = 0;  // Start time (Face Value)
    std::time_t physics_epoch = 0;  // Start time (System UTC)
    auto system_start_tp = Clock::now(); // System Real Time Start Point

    // 2. Parse Arguments
    for(int i=1; i<argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--groupbuild") builder_mode = true;
        else if (arg == "--refresh") refresh_tle = true;
        else if (arg == "--time") {
            if (i+1 < argc) {
                std::string t_str = argv[++i];

                // ROBUST PARSING (sscanf)
                int Y, M, D, h, m, s;
                if (sscanf(t_str.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) {
                    // Try combining with next argument (handle unquoted date time)
                    if (i+1 < argc) {
                        std::string next_arg = argv[i+1];
                        std::string combined = t_str + " " + next_arg;
                        if (sscanf(combined.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6) {
                            t_str = combined;
                            i++; // Consume extra argument
                        } else {
                            std::cerr << "Invalid time format. Use \"YYYY-MM-DD HH:MM:SS\"" << std::endl;
                            return 1;
                        }
                    } else {
                        std::cerr << "Invalid time format. Use \"YYYY-MM-DD HH:MM:SS\"" << std::endl;
                        return 1;
                    }
                }

                std::tm t = {};
                t.tm_year = Y - 1900;
                t.tm_mon = M - 1;
                t.tm_mday = D;
                t.tm_hour = h;
                t.tm_min = m;
                t.tm_sec = s;
                t.tm_isdst = -1;

                // 1. Display Clock (Face Value)
                display_epoch = timegm_portable(&t);

                // 2. Physics Clock (System Interpretation)
                physics_epoch = std::mktime(&t);

                sim_time = true;
                Logger::log("Simulating Time: " + t_str);
            }
        }
        else if (arg == "--lat") { if (i+1 < argc) config.lat = std::stod(argv[++i]); }
        else if (arg == "--lon") { if (i+1 < argc) config.lon = std::stod(argv[++i]); }
        else if (arg == "--alt") { if (i+1 < argc) config.alt = std::stod(argv[++i]); }
        else if (arg == "--max_sats") { if (i+1 < argc) config.max_sats = std::stoi(argv[++i]); }
        else if (arg == "--trail_mins") { if (i+1 < argc) config.trail_length_mins = std::stoi(argv[++i]); }
        else if (arg == "--maxapo") { if (i+1 < argc) config.max_apo = std::stod(argv[++i]); }
        else if (arg == "--minel") { if (i+1 < argc) config.min_el = std::stod(argv[++i]); }
        else if (arg == "--groupsel") { if (i+1 < argc) config.group_selection = argv[++i]; config.sat_selection = ""; } 
        else if (arg == "--satsel") { if (i+1 < argc) config.sat_selection = argv[++i]; } 
        else if (arg == "--visible" || arg == "-visible") {
            if (i+1 < argc) {
                std::string val = argv[++i];
                config.visible_only = (val == "true" || val == "1");
            }
        }

        // HARDWARE CONTROL FLAGS (Requires Argument)
        else if (arg == "--radio") {
            if (i+1 < argc) {
                std::string val = argv[++i];
                config.radio_control_enabled = (val == "true" || val == "1");
            }
        }
        else if (arg == "--rotator") {
            if (i+1 < argc) {
                std::string val = argv[++i];
                config.rotator_control_enabled = (val == "true" || val == "1");
            }
        }
    }

    // ENFORCE CONTROL LOGIC: Disable hardware if >1 satellite selected
    if (config.radio_control_enabled || config.rotator_control_enabled) {
        if (config.sat_selection.empty() || config.sat_selection.find(',') != std::string::npos ||
            config.sat_selection == "SUN" || config.sat_selection == "MOON") {
             // Exception: "SUN" and "MOON" are single objects but handled specially?
             // TLEManager::loadSpecificSats handles commas. If empty, it's a group.
             // If config.sat_selection is empty (group mode), DISABLE control.
             // If comma exists, DISABLE control.
             if (config.sat_selection.empty() || config.sat_selection.find(',') != std::string::npos) {
                 std::cerr << "[WARN] Radio/Rotator control disabled: Must select exactly one satellite via --satsel." << std::endl;
                 config.radio_control_enabled = false;
                 config.rotator_control_enabled = false;
             }
        }
    }

    // If not simulating time, initialize default clocks
    if (!sim_time) {
        std::time_t now_c = std::time(nullptr);
        physics_epoch = now_c; // Physics uses real time

        // Display uses Local Face Value
        std::tm local_tm;
        localtime_r(&now_c, &local_tm);
        display_epoch = timegm_portable(&local_tm);
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
    
    // SUN & MOON OVERRIDE
    // Ensure we track Sun (-1) and Moon (-2) regardless of filters, as UI expects them.
    // However, they are usually filtered out by "max_sats" if not prioritized.
    // Logic: TLEManager injects them. Main loop visibility filter might drop them.

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
        if (config.rotator_control_enabled) {
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

                         // SAFETY: Clear active_sats pointers in SharedState BEFORE destroying sats vector.
                         {
                             std::lock_guard<std::mutex> lock(state.mutex);
                             state.active_sats.clear();
                             state.rows.clear();
                             state.updated = false;
                         }

                         // Now safe to reallocate
                         sats = tle_mgr.loadGroups(new_cfg.group_selection);
                    }
                    config = new_cfg;
                    observer = Observer(config.lat, config.lon, config.alt); 
                }

                // CALCULATE PHYSICS TIME (Decoupled)
                auto elapsed_duration = Clock::now() - system_start_tp;
                long elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed_duration).count();
                std::time_t current_physics_time_t = physics_epoch + elapsed_sec;
                auto now = std::chrono::system_clock::from_time_t(current_physics_time_t);

                std::vector<DisplayRow> local_rows;
                std::vector<Satellite*> local_sats;
                
                int rejected_apo = 0;
                int rejected_el = 0;
                int rejected_vis = 0;

                int selected_norad_id = web_server.getSelectedNoradId();

                for(auto& sat : sats) {
                    if(!running) break;
                    
                    // 1. Strict Decay Filter: Satellites below 80km are considered decayed/invalid
                    if (sat.getApogeeKm() < 80.0) {
                        continue;
                    }

                    auto [pos, vel] = sat.propagate(now);
                    auto look = observer.calculateLookAngle(pos, now);
                    double rrate = observer.calculateRangeRate(pos, vel, now);

                    // ROTATOR LOGIC (Always run for selected sat, regardless of display filters)
                    if (rotator && rotator->isConnected() && sat.getNoradId() == selected_norad_id) {
                        if (look.elevation >= config.rotator_min_el) {
                            rotator->setPosition(look.azimuth, look.elevation);
                        }
                    }

                    // 2. Visibility Calculation
                    auto state = VisibilityCalculator::calculateState(pos, observer.getPositionECI(now), now, look.elevation);

                    // 3. User Filters

                    // VISIBILITY FILTER
                    // If visible_only is TRUE, we skip if NOT visible.
                    if (config.visible_only && state != VisibilityCalculator::State::VISIBLE) {
                        rejected_vis++;
                        continue;
                    }

                    // MIN ELEVATION FILTER
                    if (look.elevation < config.min_el) {
                        rejected_el++;
                        continue;
                    }

                    // MAX APOGEE FILTER
                    if (config.max_apo > 0 && sat.getApogeeKm() > config.max_apo) {
                        rejected_apo++;
                        continue;
                    }

                    // Flare Calculation (Only relevant if visible, but calculate anyway for status)
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
                    // DO NOT push to local_sats yet. We are filtering/sorting local_rows first.
                    // We must rebuild local_sats from local_rows after filtering to ensure synchronization.
                }
                
                // DIAGNOSTICS: If empty list, report why
                if(local_rows.empty() && !sats.empty()) {
                     // Only log periodically to avoid spam, or handle in UI
                     // For now, we rely on console output from main
                }
                
                if (!running) break;

                // STABLE SORT: Prevents flickering
                std::stable_sort(local_rows.begin(), local_rows.end(), [](const DisplayRow& a, const DisplayRow& b) { return a.el > b.el; });
                
                // Enforce max_sats but PRESERVE Sun/Moon
                size_t limit = (config.max_sats > 0) ? (size_t)config.max_sats : 5000;

                if (local_rows.size() > limit) {
                    std::vector<DisplayRow> kept;
                    std::vector<DisplayRow> others;
                    kept.reserve(limit);
                    others.reserve(local_rows.size());

                    // Prioritize Sun/Moon
                    for(const auto& r : local_rows) {
                        if (r.norad_id == -1 || r.norad_id == -2) kept.push_back(r);
                        else others.push_back(r);
                    }
                    // Fill remaining
                    for(const auto& r : others) {
                        if (kept.size() < limit) kept.push_back(r);
                        else break;
                    }
                    local_rows = kept;
                    // Re-sort final list
                    std::stable_sort(local_rows.begin(), local_rows.end(), [](const DisplayRow& a, const DisplayRow& b) { return a.el > b.el; });
                }

                // REBUILD ACTIVE SATS POINTERS TO MATCH FILTERED ROWS
                // This ensures WebServer JSON (which might use active_sats for details)
                // and UI are perfectly synchronized.
                local_sats.clear();
                local_sats.reserve(local_rows.size());

                // Create a lookup map for speed? Or just brute force?
                // Brute force search in 'sats' for each 'row' is O(N*M). N=100, M=5000. 500,000 ops. Fine.
                // Better: Create a map of ID -> Satellite* first.
                // Even Better: We know 'sats' indices? No.
                // Just iterate:
                for(const auto& r : local_rows) {
                    // Find sat with r.norad_id in 'sats'
                    for(auto& s : sats) {
                        if (s.getNoradId() == r.norad_id) {
                            local_sats.push_back(&s);
                            break;
                        }
                    }
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

            // CALCULATE CLOCKS
            auto elapsed_duration = Clock::now() - system_start_tp;
            long elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed_duration).count();

            // 1. Physics Time (UTC-aligned)
            std::time_t physics_tt = physics_epoch + elapsed_sec;
            auto physics_now = std::chrono::system_clock::from_time_t(physics_tt);

            // 2. Display Time (Face Value-aligned)
            std::time_t display_tt = display_epoch + elapsed_sec;

            // CONSTRUCT STRING
            std::string time_display_str;
            {
                std::tm tm_display;
                gmtime_r(&display_tt, &tm_display);
                char t_buf[64];
                std::strftime(t_buf, sizeof(t_buf), "%Y-%m-%d %H:%M:%S LOC", &tm_display);
                time_display_str = std::string(t_buf);
            }

            std::vector<DisplayRow> current_rows;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                current_rows = state.rows;
                if (state.updated) {
                    web_server.updateData(current_rows, state.active_sats, config, physics_now, time_display_str);
                }
            }
            display.update(current_rows, observer, physics_now, sats.size(), current_rows.size(), !config.visible_only, config.min_el, time_display_str);
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
