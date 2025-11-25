#include "tle_manager.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <curl/curl.h>
#include <algorithm>
#include <sys/stat.h>
#include <ctime>

namespace ve {
    TLEManager::TLEManager(const std::string& cache_dir) : cache_dir_(cache_dir) {
        if (!std::filesystem::exists(cache_dir)) std::filesystem::create_directories(cache_dir);
        cache_file_path_ = cache_dir + "/active.txt";
    }
    void TLEManager::clearCache() { if (std::filesystem::exists(cache_file_path_)) std::filesystem::remove(cache_file_path_); }
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
    bool TLEManager::isCacheFresh() {
        struct stat attr;
        if (stat(cache_file_path_.c_str(), &attr) != 0) return false;
        std::time_t mod_time = attr.st_mtime;
        std::tm* file_tm = std::localtime(&mod_time);
        int file_yday = file_tm->tm_yday; int file_year = file_tm->tm_year;
        auto now = Clock::now(); std::time_t now_t = Clock::to_time_t(now); std::tm* now_tm = std::localtime(&now_t);
        if (file_year == now_tm->tm_year && file_yday == now_tm->tm_yday) {
            std::cout << "Cache is fresh (from today)." << std::endl;
            return true;
        }
        std::cout << "Cache is old. Refreshing." << std::endl;
        return false;
    }
    bool TLEManager::downloadFile(const std::string& url, const std::string& dest_path) {
        CURL* curl = curl_easy_init(); if (!curl) return false;
        std::string readBuffer;
        std::cout << "Downloading TLEs..." << std::endl;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "VisibleEphemeris/12.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        CURLcode res = curl_easy_perform(curl);
        long http_code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK || readBuffer.empty()) {
            std::cerr << "Download failed: " << curl_easy_strerror(res) << " HTTP: " << http_code << std::endl;
            return false;
        }
        std::ofstream outfile(dest_path); outfile << readBuffer; outfile.close();
        return true;
    }
    std::vector<Satellite> TLEManager::parseFile(const std::string& filepath) {
        std::vector<Satellite> sats; std::ifstream file(filepath); std::string line, name, l1, l2;
        if (!file.is_open()) return sats;
        sats.reserve(12000);
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
    std::vector<Satellite> TLEManager::loadSatellites(const std::string& url) {
        if (!isCacheFresh()) {
            downloadFile(url, cache_file_path_);
        }
        return parseFile(cache_file_path_);
    }
}
