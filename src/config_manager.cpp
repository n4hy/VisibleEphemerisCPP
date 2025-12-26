#include "config_manager.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <algorithm>

namespace ve {
    ConfigManager::ConfigManager(const std::string& filename) : filename_(filename) {}
    bool ConfigManager::hasConfig() const { return std::filesystem::exists(filename_); }

    std::string clean(const std::string& str) {
        std::string s = str;
        // Remove Control Chars
        s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
        s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
        
        // Trim Whitespace
        size_t first = s.find_first_not_of(" \t");
        if (std::string::npos == first) return "";
        size_t last = s.find_last_not_of(" \t");
        s = s.substr(first, (last - first + 1));
        
        // NEW: Trim Quotes if they wrap the string
        if (s.size() >= 2) {
            if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')) {
                s = s.substr(1, s.size() - 2);
            }
        }
        return s;
    }

    std::map<std::string, std::string> ConfigManager::parse() {
        std::map<std::string, std::string> data;
        std::ifstream file(filename_);
        std::string line;
        while(std::getline(file, line)) {
            size_t delim = line.find(':');
            if (delim != std::string::npos) {
                std::string key = clean(line.substr(0, delim));
                std::string val = clean(line.substr(delim + 1));
                data[key] = val;
            }
        }
        return data;
    }

    AppConfig ConfigManager::load() {
        AppConfig cfg;
        if (!hasConfig()) return cfg; 
        auto data = parse();
        try {
            if (data.count("lat")) cfg.lat = std::stod(data["lat"]);
            if (data.count("lon")) cfg.lon = std::stod(data["lon"]);
            if (data.count("alt")) cfg.alt = std::stod(data["alt"]);
            if (data.count("max_sats")) cfg.max_sats = std::stoi(data["max_sats"]);
            if (data.count("min_el")) cfg.min_el = std::stod(data["min_el"]);
            if (data.count("max_apo")) cfg.max_apo = std::stod(data["max_apo"]);
            if (data.count("trail_length_mins")) cfg.trail_length_mins = std::stoi(data["trail_length_mins"]);
            
            if (data.count("group_selection")) {
                cfg.group_selection = data["group_selection"];
                std::cout << "[CONFIG] Target Group: [" << cfg.group_selection << "]" << std::endl;
            }
            
            if (data.count("sat_selection")) cfg.sat_selection = data["sat_selection"];

            // Visibility Setting (formerly radio_mode/show_all_visible)
            if (data.count("show_all")) cfg.show_all = (data["show_all"] == "true" || data["show_all"] == "1");

            // Hardware Control Settings
            if (data.count("radio_control")) cfg.radio_control_enabled = (data["radio_control"] == "true" || data["radio_control"] == "1");
            if (data.count("rotator_control")) cfg.rotator_control_enabled = (data["rotator_control"] == "true" || data["rotator_control"] == "1");

            // Legacy Backwards Compatibility
            if (data.count("show_all_visible")) cfg.show_all = (data["show_all_visible"] == "true" || data["show_all_visible"] == "1");
            if (data.count("radio_mode")) cfg.show_all = (data["radio_mode"] == "true" || data["radio_mode"] == "1");
            if (data.count("rotator_enabled")) cfg.rotator_control_enabled = (data["rotator_enabled"] == "true" || data["rotator_enabled"] == "1");

            if (data.count("rotator_host")) cfg.rotator_host = data["rotator_host"];
            if (data.count("rotator_port")) cfg.rotator_port = std::stoi(data["rotator_port"]);
            if (data.count("rotator_min_el")) cfg.rotator_min_el = std::stod(data["rotator_min_el"]);
        } catch(...) {
            std::cerr << "[CONFIG] Error parsing config.yaml" << std::endl;
        }
        return cfg;
    }
    
    void ConfigManager::save(const AppConfig& config) {
        std::ofstream file(filename_);
        file << "lat: " << config.lat << "\n";
        file << "lon: " << config.lon << "\n";
        file << "alt: " << config.alt << "\n";
        file << "max_sats: " << config.max_sats << "\n";
        file << "min_el: " << config.min_el << "\n";
        file << "max_apo: " << config.max_apo << "\n";
        file << "trail_length_mins: " << config.trail_length_mins << "\n";
        file << "group_selection: " << config.group_selection << "\n";
        file << "sat_selection: " << config.sat_selection << "\n";
        file << "show_all: " << (config.show_all ? "true" : "false") << "\n";

        file << "radio_control: " << (config.radio_control_enabled ? "true" : "false") << "\n";
        file << "rotator_control: " << (config.rotator_control_enabled ? "true" : "false") << "\n";

        file << "rotator_host: " << config.rotator_host << "\n";
        file << "rotator_port: " << config.rotator_port << "\n";
        file << "rotator_min_el: " << config.rotator_min_el << "\n";

        file.close();
    }
}
