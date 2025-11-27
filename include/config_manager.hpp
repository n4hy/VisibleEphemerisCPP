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
        bool show_all_visible = false;
        std::string group_selection = "active"; 
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
