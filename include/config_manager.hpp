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
