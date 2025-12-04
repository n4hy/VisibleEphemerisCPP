#pragma once
#include <string>
#include <map>
#include <vector>

namespace ve {
    struct RadioInfo {
        long uplink_freq;
        long downlink_freq;
        std::string mode;
    };

    class FrequencyManager {
    public:
        FrequencyManager(const std::string& filename);
        bool hasFrequencies(const std::string& sat_name) const;
        RadioInfo getFrequencies(const std::string& sat_name) const;
        void reload();

    private:
        std::string filename_;
        std::map<std::string, RadioInfo> freq_map_;
        void load();
    };
}
