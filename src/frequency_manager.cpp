#include "frequency_manager.hpp"
#include "logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

namespace ve {
    FrequencyManager::FrequencyManager(const std::string& filename) : filename_(filename) {
        load();
    }

    void FrequencyManager::reload() {
        load();
    }

    void FrequencyManager::load() {
        freq_map_.clear();
        std::ifstream file(filename_);
        if (!file.is_open()) {
            Logger::log("WARNING: Could not open frequency table: " + filename_);
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            // Format: Name,Uplink,Downlink,Mode
            // Example: ISS,145800000,145990000,FM
            if (line.empty() || line[0] == '#') continue;

            std::stringstream ss(line);
            std::string name, ul_str, dl_str, mode;

            if (std::getline(ss, name, ',') &&
                std::getline(ss, ul_str, ',') &&
                std::getline(ss, dl_str, ',') &&
                std::getline(ss, mode, ',')) {

                try {
                    // Normalize name: Upper case for matching
                    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
                    // Trim name
                    size_t first = name.find_first_not_of(" \t");
                    if (first != std::string::npos) {
                        size_t last = name.find_last_not_of(" \t");
                        name = name.substr(first, (last-first+1));
                    }

                    RadioInfo info;
                    info.uplink_freq = std::stol(ul_str);
                    info.downlink_freq = std::stol(dl_str);
                    info.mode = mode; // Trim mode?

                    freq_map_[name] = info;
                } catch (...) {
                    continue;
                }
            }
        }
        Logger::log("Loaded frequencies for " + std::to_string(freq_map_.size()) + " satellites");
    }

    bool FrequencyManager::hasFrequencies(const std::string& sat_name) const {
        std::string n = sat_name;
        std::transform(n.begin(), n.end(), n.begin(), ::toupper);
        return freq_map_.count(n);
    }

    RadioInfo FrequencyManager::getFrequencies(const std::string& sat_name) const {
        std::string n = sat_name;
        std::transform(n.begin(), n.end(), n.begin(), ::toupper);
        if (freq_map_.count(n)) return freq_map_.at(n);
        return {0, 0, ""};
    }
}
