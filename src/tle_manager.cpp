#include "tle_manager.hpp"
#include "logger.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <curl/curl.h>
#include <algorithm>
#include <sys/stat.h>
#include <ctime>
#include <set>
#include <sstream>

namespace ve {
    TLEManager::TLEManager(const std::string& cache_dir) : cache_dir_(cache_dir) {
        if (!std::filesystem::exists(cache_dir)) std::filesystem::create_directories(cache_dir);
    }

    void TLEManager::clearCache() { 
        for (const auto& entry : std::filesystem::directory_iterator(cache_dir_)) 
            std::filesystem::remove(entry.path());
    }

    size_t TLEManager::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string TLEManager::trim(const std::string& str) {
        std::string s = str;
        s.erase(s.find_last_not_of(" \n\r\t") + 1);
        s.erase(0, s.find_first_not_of(" \n\r\t"));
        return s;
    }

    bool TLEManager::isCacheFresh(const std::string& filepath) {
        struct stat attr;
        if (stat(filepath.c_str(), &attr) != 0) return false;
        std::time_t mod_time = attr.st_mtime;
        std::time_t now = std::time(nullptr);
        return (std::difftime(now, mod_time) < 86400.0); 
    }

    bool TLEManager::downloadFile(const std::string& url, const std::string& dest_path) {
        CURL* curl = curl_easy_init(); 
        if (!curl) return false;
        std::string readBuffer;
        Logger::log("Downloading: " + url);
        std::cout << "Downloading " << url << "..." << std::endl; 
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "VisibleEphemeris/12.65");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        CURLcode res = curl_easy_perform(curl);
        long http_code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK || readBuffer.empty()) {
            Logger::log("Download failed: " + std::string(curl_easy_strerror(res)));
            return false;
        }
        std::ofstream outfile(dest_path); 
        outfile << readBuffer; 
        outfile.close();
        return true;
    }

    std::vector<Satellite> TLEManager::parseFile(const std::string& filepath) {
        std::vector<Satellite> sats; 
        std::ifstream file(filepath); 
        std::string line, name, l1, l2;
        if (!file.is_open()) return sats;
        while (std::getline(file, line)) {
            line = trim(line); if (line.length() < 2) continue;
            if (line.substr(0, 2) == "1 " && !name.empty()) {
                l1 = line;
                if (std::getline(file, l2)) {
                    l2 = trim(l2);
                    if (l2.substr(0, 2) == "2 ") {
                        name.erase(name.find_last_not_of(" \n\r\t")+1);
                        try { sats.emplace_back(name, l1, l2); } catch(...) {}
                        name = "";
                    }
                }
            } else { name = line; }
        }
        return sats;
    }

    std::string TLEManager::getUrlForGroup(const std::string& group) {
        if (group == "active") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=active&FORMAT=tle";
        if (group == "visual") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=visual&FORMAT=tle";
        if (group == "stations") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=stations&FORMAT=tle";
        if (group == "weather" || group == "noaa" || group == "goes") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=weather&FORMAT=tle";
        if (group == "resource" || group == "sarsat" || group == "dmc") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=resource&FORMAT=tle";
        if (group == "geo") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=geo&FORMAT=tle";
        if (group == "intelsat") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=intelsat&FORMAT=tle";
        if (group == "iridium") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=iridium&FORMAT=tle";
        if (group == "starlink") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=starlink&FORMAT=tle";
        if (group == "oneweb") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=oneweb&FORMAT=tle";
        if (group == "orbcomm") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=orbcomm&FORMAT=tle";
        if (group == "globalstar") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=globalstar&FORMAT=tle";
        if (group == "amateur") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=tle";
        if (group == "satnogs") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=satnogs&FORMAT=tle";
        if (group == "gnss" || group == "gps" || group == "galileo" || group == "beidou") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=gnss&FORMAT=tle";
        if (group == "science" || group == "geodetic") return "https://celestrak.org/NORAD/elements/gp.php?GROUP=science&FORMAT=tle";
        Logger::log("Unknown group: " + group + ", defaulting to active");
        return "https://celestrak.org/NORAD/elements/gp.php?GROUP=active&FORMAT=tle";
    }

    std::vector<Satellite> TLEManager::loadGroups(const std::string& groups_list_str) {
        std::vector<Satellite> all_sats;
        std::set<int> loaded_ids;
        std::stringstream ss(groups_list_str);
        std::string segment;
        while(std::getline(ss, segment, ',')) {
            segment = trim(segment);
            if (segment.empty()) continue;
            std::string url = getUrlForGroup(segment);
            std::string filename = cache_dir_ + "/" + segment + ".txt";
            if (!isCacheFresh(filename)) downloadFile(url, filename);
            std::vector<Satellite> group_sats = parseFile(filename);
            for (auto& sat : group_sats) {
                int id = sat.getNoradId();
                if (loaded_ids.find(id) == loaded_ids.end()) {
                    loaded_ids.insert(id);
                    all_sats.push_back(std::move(sat));
                }
            }
        }
        if (all_sats.empty()) Logger::log("No satellites loaded from groups: " + groups_list_str);
        return all_sats;
    }
}
