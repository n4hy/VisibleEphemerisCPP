#include "frequency_manager.hpp"
#include "logger.hpp"
#include <fstream>
#include <iostream>
#include <curl/curl.h>
#include <algorithm>
#include <sstream>

// We need a JSON parser. Since we don't have nlohmann/json in the file list and installing it might be tricky in this environment if not present,
// I will write a very simple ad-hoc parser for the SatNOGS structure or use a regex approach since the structure is flat list of objects.
// SatNOGS JSON is a list of objects: [{"uuid":..., "norad_cat_id": 12345, "uplink_low": 123, ...}, ...]
// I'll implement a basic parser for this specific format to avoid heavy dependencies if not available.
// Actually, I can check if nlohmann/json is available or if I should assume it.
// Given the environment, writing a lightweight parser for this specific task is safer.

namespace ve {
    // --- Helper JSON Parser ---
    std::string getJsonValue(const std::string& obj, const std::string& key) {
        std::string k = "\"" + key + "\":";
        size_t pos = obj.find(k);
        if (pos == std::string::npos) return "";
        pos += k.length();

        // Skip whitespace
        while (pos < obj.length() && (obj[pos] == ' ' || obj[pos] == '\t')) pos++;

        if (pos >= obj.length()) return "";

        if (obj[pos] == '"') { // String
            size_t end = obj.find('"', pos+1);
            if (end == std::string::npos) return "";
            return obj.substr(pos+1, end-pos-1);
        } else if (obj[pos] == 'n') { // null
            return "";
        } else { // Number or boolean
            size_t end = obj.find_first_of(",}", pos);
            if (end == std::string::npos) return obj.substr(pos);
            return obj.substr(pos, end-pos);
        }
    }

    FrequencyManager::FrequencyManager(const std::string& cache_file) : cache_file_(cache_file) {
        // Don't auto-load in constructor to allow main to trigger update first
    }

    size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    bool FrequencyManager::download() {
        std::cout << "[SATNOGS] Downloading database from https://db.satnogs.org/api/transmitters/?format=json ..." << std::endl;
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::string readBuffer;
        curl_easy_setopt(curl, CURLOPT_URL, "https://db.satnogs.org/api/transmitters/?format=json");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L); // 30s timeout
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "VisibleEphemeris/1.0"); // Some APIs block empty UA

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK && !readBuffer.empty()) {
            std::cout << "[SATNOGS] Download complete: " << readBuffer.size() << " bytes." << std::endl;
            std::ofstream outfile(cache_file_);
            outfile << readBuffer;
            outfile.close();
            Logger::log("SatNOGS DB Downloaded: " + std::to_string(readBuffer.size()) + " bytes");
            parseJson(readBuffer);
            return true;
        } else {
            std::cout << "[SATNOGS] Download FAILED: " << curl_easy_strerror(res) << std::endl;
            Logger::log("SatNOGS DB Download Failed: " + std::string(curl_easy_strerror(res)));
            return false;
        }
    }

    void FrequencyManager::loadFromCache() {
        std::ifstream file(cache_file_);
        if (file.is_open()) {
            std::cout << "[SATNOGS] Loading database from local cache (" << cache_file_ << ")..." << std::endl;
            std::stringstream buffer;
            buffer << file.rdbuf();
            Logger::log("Loaded SatNOGS DB from cache");
            parseJson(buffer.str());
        } else {
            std::cout << "[SATNOGS] No local cache found." << std::endl;
            Logger::log("No SatNOGS cache found.");
        }
    }

    bool FrequencyManager::updateDatabase() {
        if (download()) return true;
        loadFromCache();
        return false;
    }

    void FrequencyManager::parseJson(const std::string& json_data) {
        std::lock_guard<std::mutex> lock(mutex_);
        db_.clear();

        // Very basic array parser
        size_t pos = 0;
        int count = 0;
        while ((pos = json_data.find('{', pos)) != std::string::npos) {
            size_t end = json_data.find('}', pos);
            if (end == std::string::npos) break;

            std::string obj = json_data.substr(pos, end-pos+1);
            pos = end + 1;

            try {
                std::string nid_str = getJsonValue(obj, "norad_cat_id");
                if (nid_str.empty()) continue;
                int nid = std::stoi(nid_str);

                std::string ul_str = getJsonValue(obj, "uplink_low");
                std::string dl_str = getJsonValue(obj, "downlink_low");
                std::string mode = getJsonValue(obj, "mode");
                std::string status = getJsonValue(obj, "status");
                std::string desc = getJsonValue(obj, "description"); // Not always present

                Transmitter tx;
                tx.uplink_low = ul_str.empty() ? 0 : std::stol(ul_str);
                tx.downlink_low = dl_str.empty() ? 0 : std::stol(dl_str);
                tx.mode = mode;
                tx.description = desc;
                tx.active = (status == "active");

                db_[nid].push_back(tx);
                count++;
            } catch (...) {
                continue;
            }
        }
        Logger::log("Parsed " + std::to_string(count) + " transmitters for " + std::to_string(db_.size()) + " satellites");
    }

    bool FrequencyManager::hasTransmitter(int norad_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return db_.count(norad_id);
    }

    Transmitter FrequencyManager::getBestTransmitter(int norad_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_.find(norad_id) == db_.end()) return {0,0,"","",false};

        const auto& list = db_.at(norad_id);

        // Priority Logic

        // 1. Active Weather APT (137 MHz range)
        for (const auto& tx : list) {
            if (tx.active && tx.downlink_low >= 137000000 && tx.downlink_low <= 138000000 && tx.mode.find("FM") != std::string::npos) return tx;
        }

        // 2. Active Amateur FM Voice
        for (const auto& tx : list) {
            if (tx.active && tx.mode.find("FM") != std::string::npos &&
               (tx.description.find("Voice") != std::string::npos || tx.description.find("Repeater") != std::string::npos)) return tx;
        }

        // 3. Active Any FM
        for (const auto& tx : list) {
            if (tx.active && tx.mode.find("FM") != std::string::npos) return tx;
        }

        // 4. Active SSB/CW (Linear)
        for (const auto& tx : list) {
            if (tx.active && (tx.mode.find("SSB") != std::string::npos || tx.mode.find("CW") != std::string::npos)) return tx;
        }

        // 5. Any Active with Downlink
        for (const auto& tx : list) {
            if (tx.active && tx.downlink_low > 0) return tx;
        }

        // 6. Anything with Downlink
        for (const auto& tx : list) {
            if (tx.downlink_low > 0) return tx;
        }

        return {0,0,"","",false};
    }
}
