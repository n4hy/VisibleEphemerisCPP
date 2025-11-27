#include "config_manager.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>

namespace ve {
    ConfigManager::ConfigManager(const std::string& filename) : filename_(filename) {}
    bool ConfigManager::hasConfig() const { return std::filesystem::exists(filename_); }
    std::map<std::string, std::string> ConfigManager::parse() {
        std::map<std::string, std::string> data;
        std::ifstream file(filename_);
        std::string line;
        while(std::getline(file, line)) {
            size_t delim = line.find(':');
            if (delim != std::string::npos) {
                std::string key = line.substr(0, delim);
                std::string val = line.substr(delim + 1);
                key.erase(0, key.find_first_not_of(" \t")); key.erase(key.find_last_not_of(" \t") + 1);
                val.erase(0, val.find_first_not_of(" \t")); val.erase(val.find_last_not_of(" \t") + 1);
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
            if (data.count("group_selection")) cfg.group_selection = data["group_selection"];
            if (data.count("show_all_visible")) cfg.show_all_visible = (data["show_all_visible"] == "true" || data["show_all_visible"] == "1");
        } catch(...) {}
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
        file << "show_all_visible: " << (config.show_all_visible ? "true" : "false") << "\n";
        file.close();
    }
}
