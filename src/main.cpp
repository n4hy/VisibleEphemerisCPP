#include <iostream>
#include <vector>
#include <thread>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <unordered_map>
#include "satellite.hpp"
#include "observer.hpp"
#include "visibility.hpp"
#include "tle_manager.hpp"
#include "display.hpp"
#include "web_server.hpp"
#include "text_server.hpp"
#include "physics_server.hpp"
#include "config_manager.hpp"
#include "pass_predictor.hpp"
#include "thread_pool.hpp"
#include "logger.hpp"
#include "rotator.hpp"

using namespace ve;

void print_help() {
    std::cout << "Usage: ./VisibleEphemeris [OPTIONS]\n\n"
              << "Options:\n"
              << "  --help, -h        Show help\n"
              << "  --lat <deg>       Override Latitude\n"
              << "  --lon <deg>       Override Longitude\n"
              << "  --alt <km>        Override Altitude (km)\n"
              << "  --minel <deg>     Minimum elevation for display (default 0)\n"
              << "  --maxsats <N>     Override Max Satellites (alias: --max_sats)\n"
              << "  --trail_mins <N>  Override Trail Length (+/- minutes)\n"
              << "  --maxapo <km>     Maximum apogee filter (km). -1 disables.\n"
              << "  --refresh         Force fresh TLE\n"
              << "  --groupsel <list> Comma-separated groups (e.g. \"amateur,weather,stations\")\n"
              << "  --satsel <list>   Comma-separated Satellite Names (Overrules groupsel)\n"
              << "  --visible         Optical Mode: show only naked-eye visible sats\n"
              << "  --no-visible      Radio Mode: show ALL sats (color-coded by visibility)\n"
              << "  --time <str>      Simulate UTC time (e.g. \"2019-06-15 12:00:00\"). Past\n"
              << "                    dates >24h ago fetch historical TLEs from Space-Track.\n"
              << "  --deltaT <sec>    Time increment between calculations (0.001-60, default 1)\n"
              << "  --radio <bool>    Enable radio control (true/false, requires --satsel)\n"
              << "  --rotator <bool>  Enable rotator control (true/false, requires --satsel)\n"
              << "  --groupbuild      Enter Mission Planner builder mode\n"
              << "  --port <A,B,C>    Override network ports (default: 8080,12345,12346)\n"
              << "                    Use empty values to keep defaults (e.g. --port ,,12349)\n"
              << "\nNetwork Ports (defaults):\n"
              << "  Port 8080         Web Dashboard / Mission Planner UI (HTTP)\n"
              << "  Port 12345        Terminal Mirror Server (HTTP text display)\n"
              << "  Port 12346        Physics Stream Server (TCP, JSON satellite data)\n"
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

// Helper function for batch pre-calculation
void run_precalc(std::vector<Satellite>& satellites, const Observer& obs, ThreadPool& pool, const AppConfig& cfg, std::chrono::system_clock::time_point start_time) {
    if (satellites.empty()) return;

    std::atomic<int> tasks_remaining(satellites.size());
    std::cout << "Pre-calculating passes for " << satellites.size() << " satellites (24h horizon)..." << std::endl;

    for(auto& sat : satellites) {
        pool.enqueue([&sat, obs, start_time, cfg, &tasks_remaining]() {
            PassPredictor local_predictor(obs);
            auto passes = local_predictor.predict(sat, start_time); // Default 1440 mins (24h)
            sat.setPredictedPasses(passes);
            // Also calculate initial ground track (valid for start_time)
            sat.calculateGroundTrack(start_time, cfg.trail_length_mins, 60);
            tasks_remaining--;
        });
    }

    // Blocking wait with progress
    int total = satellites.size();
    while(tasks_remaining > 0) {
        int done = total - tasks_remaining;
        if (done % 50 == 0 || done == total) {
            std::cout << "\rProgress: " << done << "/" << total << "   " << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cout << "\nPre-calculation complete." << std::endl;
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

    // Network ports (defaults)
    int port_web = 8080;
    int port_text = 12345;
    int port_physics = 12346;

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
        else if (arg == "--max_sats" || arg == "--maxsats") { if (i+1 < argc) config.max_sats = std::stoi(argv[++i]); }
        else if (arg == "--trail_mins") { if (i+1 < argc) config.trail_length_mins = std::stoi(argv[++i]); }
        else if (arg == "--maxapo" || arg == "--map_apo") { if (i+1 < argc) config.max_apo = std::stod(argv[++i]); }
        else if (arg == "--minel") { if (i+1 < argc) config.min_el = std::stod(argv[++i]); }
        else if (arg == "--groupsel") { if (i+1 < argc) config.group_selection = argv[++i]; config.sat_selection = ""; } 
        else if (arg == "--satsel") { if (i+1 < argc) config.sat_selection = argv[++i]; } 
        else if (arg == "--visible" || arg == "-visible") {
            // Bare "--visible" enables Optical Mode (matches Python/README). An optional
            // explicit boolean ("--visible true/false") is still accepted for back-compat.
            if (i+1 < argc) {
                std::string val = argv[i+1];
                if (val == "true" || val == "1")       { config.visible_only = true;  i++; }
                else if (val == "false" || val == "0") { config.visible_only = false; i++; }
                else                                    { config.visible_only = true; }
            } else {
                config.visible_only = true;
            }
        }
        else if (arg == "--no-visible") { config.visible_only = false; }

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
        else if (arg == "--deltaT") {
            if (i+1 < argc) {
                double val = std::stod(argv[++i]);
                if (val < 0.001 || val > 60.0) {
                    std::cerr << "[WARN] --deltaT must be between 0.001 and 60 seconds. Using default (1.0)." << std::endl;
                } else {
                    config.delta_t = val;
                }
            }
        }
        else if (arg == "--port") {
            if (i+1 < argc) {
                std::string port_str = argv[++i];
                // Parse comma-separated ports: A,B,C where empty values keep defaults
                std::vector<std::string> parts;
                size_t start = 0, end;
                while ((end = port_str.find(',', start)) != std::string::npos) {
                    parts.push_back(port_str.substr(start, end - start));
                    start = end + 1;
                }
                parts.push_back(port_str.substr(start));

                // Apply non-empty values to respective ports
                if (parts.size() >= 1 && !parts[0].empty()) {
                    port_web = std::stoi(parts[0]);
                }
                if (parts.size() >= 2 && !parts[1].empty()) {
                    port_text = std::stoi(parts[1]);
                }
                if (parts.size() >= 3 && !parts[2].empty()) {
                    port_physics = std::stoi(parts[2]);
                }
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
            std::cout << "Starting Mission Planner UI on port " << port_web << "..." << std::endl;
            WebServer builder_server(port_web, tle_mgr, true);
            builder_server.runBlocking(); 
            std::cout << "Configuration saved. Launching Tracker..." << std::endl;
            config = config_mgr.load(); 
        }
        
        // --- PHASE 2: TRACKER MODE ---
        std::cout << "Loading TLEs..." << std::endl;

        // Decide between live (Celestrak, today's elements) and historical (Space-Track
        // gp_history at physics_epoch) based on how far the simulated time sits from now.
        std::time_t now_real = std::time(nullptr);
        double gap_seconds = std::difftime(now_real, physics_epoch);
        bool use_historical = sim_time && (gap_seconds > 86400.0 || gap_seconds < -86400.0);
        const int hist_window_days = 10;

        std::vector<Satellite> sats;
        if (!config.sat_selection.empty()) {
             std::cout << "Loading specific satellites: " << config.sat_selection
                       << (use_historical ? " [HISTORICAL]" : "") << "..." << std::endl;
             sats = use_historical
                 ? tle_mgr.loadSpecificSatsForDate(config.sat_selection, physics_epoch, hist_window_days)
                 : tle_mgr.loadSpecificSats(config.sat_selection);
        } else {
             std::cout << "Loading TLE groups: " << config.group_selection
                       << (use_historical ? " [HISTORICAL]" : "") << "..." << std::endl;
             sats = use_historical
                 ? tle_mgr.loadGroupsForDate(config.group_selection, physics_epoch, hist_window_days)
                 : tle_mgr.loadGroups(config.group_selection);
        }
        
        if (sats.empty()) { 
            std::cerr << "ERROR: No satellites loaded! Check network or groups." << std::endl; 
            Logger::log("ERROR: No satellites loaded");
            return 1; 
        }
        Logger::log("Loaded " + std::to_string(sats.size()) + " satellites");

        WebServer web_server(port_web, tle_mgr, false);
        TextServer text_server(port_text);
        PhysicsServer physics_server(port_physics);
        
        Observer observer(config.lat, config.lon, config.alt);
        Display display; 
        display.setBlocking(true); 
        
        ThreadPool pool(4); 
        PassPredictor predictor(observer);
        
        std::unique_ptr<Rotator> rotator;
        if (config.rotator_control_enabled) {
            rotator = std::make_unique<Rotator>(config.rotator_host, config.rotator_port);
        }
        
        // Initial Pre-calculation
        run_precalc(sats, observer, pool, config, std::chrono::system_clock::from_time_t(physics_epoch));

        web_server.start();
        text_server.start();
        physics_server.start();

        auto last_calc_time = Clock::now();
        bool first_run = true;
        SharedState state;
        std::atomic<bool> running(true);

        // BACKGROUND MATH THREAD
        std::thread math_thread([&]() {
            auto last_tle_refresh = std::chrono::steady_clock::now();

            while(running) {
                // CALCULATE PHYSICS TIME (Decoupled) - with sub-second precision
                auto elapsed_duration = Clock::now() - system_start_tp;
                auto now = std::chrono::system_clock::from_time_t(physics_epoch) + elapsed_duration;

                // AUTO-REFRESH / HOT-RELOAD LOGIC
                bool perform_reload = false;
                bool force_refresh = false;

                // 1. Check Schedule (Daily TLE Update) - skipped in historical mode
                auto now_steady = std::chrono::steady_clock::now();
                if (!use_historical && now_steady - last_tle_refresh > std::chrono::hours(24)) {
                    Logger::log("Scheduled Daily TLE Refresh...");
                    perform_reload = true;
                    force_refresh = true;
                    last_tle_refresh = now_steady;
                }

                // 2. Check Config Change
                if (web_server.hasPendingConfig()) {
                    AppConfig new_cfg = web_server.popPendingConfig();
                    bool selection_changed = (new_cfg.group_selection != config.group_selection) ||
                                             (new_cfg.sat_selection != config.sat_selection);

                    config = new_cfg;
                    observer = Observer(config.lat, config.lon, config.alt);

                    if (selection_changed) {
                         Logger::log("Hot Reload: Switching selection...");
                         perform_reload = true;
                    }
                }

                if (perform_reload) {
                     if (force_refresh) tle_mgr.clearCache();

                     // SAFETY: Clear active_sats pointers in SharedState BEFORE destroying sats vector.
                     {
                         std::lock_guard<std::mutex> lock(state.mutex);
                         state.active_sats.clear();
                         state.rows.clear();
                         state.updated = false;
                     }

                     // Re-load (preserve historical vs. live routing)
                     if (!config.sat_selection.empty()) {
                          sats = use_historical
                              ? tle_mgr.loadSpecificSatsForDate(config.sat_selection, physics_epoch, hist_window_days)
                              : tle_mgr.loadSpecificSats(config.sat_selection);
                     } else {
                          sats = use_historical
                              ? tle_mgr.loadGroupsForDate(config.group_selection, physics_epoch, hist_window_days)
                              : tle_mgr.loadGroups(config.group_selection);
                     }

                     // Re-Run Pre-calc
                     run_precalc(sats, observer, pool, config, now);
                }

                std::vector<DisplayRow> local_rows;
                std::vector<Satellite*> local_sats;

                // Pre-allocate vectors to avoid reallocations
                local_rows.reserve(sats.size());
                local_sats.reserve(sats.size());

                // Build lookup map for O(1) satellite access by NORAD ID
                std::unordered_map<int, Satellite*> sat_lookup;
                sat_lookup.reserve(sats.size());
                for (auto& s : sats) {
                    sat_lookup[s.getNoradId()] = &s;
                }

                int rejected_apo = 0;
                int rejected_el = 0;
                int rejected_vis = 0;

                int selected_norad_id = web_server.getSelectedNoradId();

                for(auto& sat : sats) {
                    if(!running) break;

                    // 1. Strict Decay Filter: Satellites below threshold are considered decayed/invalid
                    if (sat.getApogeeKm() < DECAY_ALTITUDE_KM) {
                        continue;
                    }

                    // MAX APOGEE FILTER (cheap check, do early)
                    if (config.max_apo > 0 && sat.getApogeeKm() > config.max_apo) {
                        rejected_apo++;
                        continue;
                    }

                    // FAST PATH: When visible_only, skip satellites not currently above horizon
                    // This avoids expensive propagate/lookangle/visibility calculations
                    if (config.visible_only) {
                        auto passes = sat.getPredictedPasses();

                        // If we have pass data, use it to quickly check if above horizon
                        if (!passes.empty()) {
                            bool above_horizon = false;
                            bool found_past_event = false;

                            // Find most recent event before now
                            for (auto it = passes.rbegin(); it != passes.rend(); ++it) {
                                if (it->time <= now) {
                                    above_horizon = it->is_aos; // If last event was AOS, we're above horizon
                                    found_past_event = true;
                                    break;
                                }
                            }

                            // If no past event, check first future event
                            // If first future event is LOS, satellite is currently above horizon
                            if (!found_past_event) {
                                above_horizon = !passes.front().is_aos;
                            }

                            if (!above_horizon) {
                                continue; // Skip - not above horizon, can't be visible
                            }
                        }
                        // If passes is empty, fall through and compute (could be GEO or always-visible)
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
                    // When visible_only=false (radio mode): Show ALL satellites, display layer colors them
                    // When visible_only=true (optical mode): Filter to only sunlit sats above min_el

                    // VISIBILITY FILTER (only in optical mode)
                    if (config.visible_only && state != VisibilityCalculator::State::VISIBLE) {
                        rejected_vis++;
                        continue;
                    }

                    // MIN ELEVATION FILTER (only in optical mode)
                    if (config.visible_only && look.elevation < config.min_el) {
                        rejected_el++;
                        continue;
                    }

                    // Flare Calculation (Only relevant if visible, but calculate anyway for status)
                    int flare_status = 0;
                    if (state == VisibilityCalculator::State::VISIBLE) {
                        flare_status = VisibilityCalculator::checkFlare(pos, observer.getPositionECI(now), VisibilityCalculator::getSunPositionECI(now), sat.getApogeeKm());
                    }

                    std::string next_event_str = "--";
                    auto passes = sat.getPredictedPasses();

                    // Find first future event
                    for(const auto& p : passes) {
                        long diff = std::chrono::duration_cast<std::chrono::seconds>(p.time - now).count();
                        if (diff > 0) {
                             int mm = diff / 60;
                             int ss = diff % 60;

                             std::stringstream ts;
                             ts << (p.is_aos ? "AOS " : "LOS ");

                             if (mm >= 60) {
                                 int hh = mm / 60;
                                 mm = mm % 60;
                                 ts << hh << "h " << mm << "m";
                             } else {
                                 ts << mm << "m " << ss << "s";
                             }
                             next_event_str = ts.str();
                             break;
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
                // When visible_only=false (radio mode), show ALL satellites - no max_sats limit
                // The user explicitly wants to see the entire group
                size_t limit = config.visible_only
                    ? ((config.max_sats > 0) ? (size_t)config.max_sats : 5000)
                    : 50000;  // Effectively unlimited for radio mode

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

                // Use pre-built lookup map for O(1) access
                for(const auto& r : local_rows) {
                    auto it = sat_lookup.find(r.norad_id);
                    if (it != sat_lookup.end()) {
                        local_sats.push_back(it->second);
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.rows = local_rows;
                    state.active_sats = local_sats;
                    state.updated = true;
                }

                // Sleep for delta_t seconds, checking running flag periodically
                int total_ms = static_cast<int>(config.delta_t * 1000);
                int elapsed = 0;
                while (elapsed < total_ms && running) {
                    int sleep_chunk = std::min(50, total_ms - elapsed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_chunk));
                    elapsed += sleep_chunk;
                }
            }
        });

        // MAIN UI LOOP
        while (true) {
            int timeout_ms = std::max(1, static_cast<int>(config.delta_t * 1000));
            display.setBlocking(true, timeout_ms);
            auto input_res = display.handleInput();
            if (input_res == Display::InputResult::SAVE_AND_QUIT) { config_mgr.save(config); running=false; break; }
            else if (input_res == Display::InputResult::QUIT_NO_SAVE) { running=false; break; }

            // CALCULATE CLOCKS - with sub-second precision
            auto elapsed_duration = Clock::now() - system_start_tp;
            long elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed_duration).count();

            // 1. Physics Time (UTC-aligned) - sub-second precision
            auto physics_now = std::chrono::system_clock::from_time_t(physics_epoch) + elapsed_duration;

            // 2. Display Time (Face Value-aligned) - for time string display only
            std::time_t display_tt = display_epoch + elapsed_sec;

            // CONSTRUCT STRING - with sub-second display
            std::string time_display_str;
            {
                std::tm tm_display;
                gmtime_r(&display_tt, &tm_display);
                char t_buf[64];
                int ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_duration).count() % 1000;
                std::strftime(t_buf, sizeof(t_buf), "%Y-%m-%d %H:%M:%S", &tm_display);
                time_display_str = std::string(t_buf) + "." + std::to_string(ms / 100) + " LOC";
            }

            std::vector<DisplayRow> current_rows;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                current_rows = state.rows;
                if (state.updated) {
                    web_server.updateData(current_rows, state.active_sats, config, physics_now, time_display_str);
                }
            }

            // Terminal display uses max_sats limit; web gets full list
            std::vector<DisplayRow> terminal_rows = current_rows;
            size_t terminal_limit = (config.max_sats > 0) ? (size_t)config.max_sats : 5000;
            if (terminal_rows.size() > terminal_limit) {
                terminal_rows.resize(terminal_limit);
            }
            display.update(terminal_rows, observer, physics_now, sats.size(), current_rows.size(), !config.visible_only, config.min_el, time_display_str);
            text_server.updateData(display.getLastFrame());
            physics_server.updateData(display.getLastFrame());
        }

        web_server.stop();
        text_server.stop();
        physics_server.stop();
        if(math_thread.joinable()) math_thread.join();
        Logger::log("Shutdown Complete");

    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
