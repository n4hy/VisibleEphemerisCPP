// config_manager.hpp - Application configuration.
//
// AppConfig is the single struct of runtime settings (observer location,
// display filters, group/satellite selection, hardware-control options, update
// rate, and the High-Precision Orbit Propagator switches). ConfigManager reads
// and writes it as a simple YAML-ish key/value file (config.yaml). Defaults
// here are the authoritative fallbacks when no file or CLI override is present.
#pragma once
#include <string>
#include <map>

namespace ve {
    struct AppConfig {
        double lat = 0.0;
        double lon = 0.0;
        double alt = 0.0;
        int max_sats = 100;
        double min_el = 0.0;
        double max_apo = -1.0;
        int trail_length_mins = 5;
        bool visible_only = false; // true = Show ONLY Visible; false = Show All (subject to other filters)
        std::string group_selection = "active"; 
        std::string sat_selection = ""; // Specific Satellite Names

        // Hardware Control Settings
        bool radio_control_enabled = false;
        bool rotator_control_enabled = false;

        std::string rotator_host = "localhost";
        int rotator_port = 4533;
        double rotator_min_el = 0.0;

        // Runtime-Only: Time Offset for Display (Input Local vs UTC)
        long manual_time_offset = 0;

        // Time increment between full calculations (seconds)
        double delta_t = 1.0;

        // High-Precision Orbit Propagator (HPOP) settings. When enabled, the
        // numerical force-model integrator replaces SGP4 for state propagation.
        bool high_precision = false;     // --hpop
        int  hpop_degree = 20;           // geopotential degree/order (<= 20)
        bool hpop_drag = true;           // --no-drag to disable
        bool hpop_srp = true;            // --no-srp to disable
        bool hpop_thirdbody = true;      // --no-thirdbody to disable Sun/Moon
    };

    class ConfigManager {
    public:
        ConfigManager(const std::string& filename);
        AppConfig load();
        void save(const AppConfig& config);
        bool hasConfig() const;
    private:
        std::string filename_;
        std::map<std::string, std::string> parse();
    };
}
