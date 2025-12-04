#pragma once
#include <string>
#include <map>
#include <vector>
#include <mutex>

namespace ve {
    struct Transmitter {
        long uplink_low;
        long downlink_low;
        std::string mode;
        std::string description;
        bool active;
    };

    class FrequencyManager {
    public:
        FrequencyManager(const std::string& cache_file);

        // Downloads DB if accessible, else loads cache. Returns true if updated.
        bool updateDatabase();

        bool hasTransmitter(int norad_id) const;
        Transmitter getBestTransmitter(int norad_id) const;

    private:
        std::string cache_file_;
        std::map<int, std::vector<Transmitter>> db_;
        mutable std::mutex mutex_;

        void loadFromCache();
        bool download();
        void parseJson(const std::string& json_data);
    };
}
