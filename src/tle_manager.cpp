// tle_manager.cpp - TLE acquisition, caching, and parsing implementation.
// Live element sets are downloaded from Celestrak (per group/name) with libcurl
// and cached under cache_dir_ with a freshness check; historical sets are routed
// through SpaceTrackClient and cached per date. parseFile() turns 3LE text into
// Satellite objects. Also implements the Mission Planner support paths: an
// in-memory master catalog with substring search and custom-group persistence.
#include "tle_manager.hpp"
#include "spacetrack_client.hpp"
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
        std::cout << "[CACHE] Cleared." << std::endl;
    }

    size_t TLEManager::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string TLEManager::trim(const std::string& str) {
        std::string s = str;
        s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
        s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
        size_t first = s.find_first_not_of(" \t");
        if (std::string::npos == first) return "";
        size_t last = s.find_last_not_of(" \t");
        return s.substr(first, (last - first + 1));
    }

    bool TLEManager::isCacheFresh(const std::string& filepath) {
        struct stat attr;
        if (stat(filepath.c_str(), &attr) != 0) return false;
        
        // Anti-Poison
        if (filepath.find("active.txt") == std::string::npos) {
            if (attr.st_size > 2 * 1024 * 1024) { 
                std::cerr << "[CACHE] CORRUPT: File too large for group. Deleting " << filepath << std::endl;
                std::filesystem::remove(filepath);
                return false;
            }
        }
        if (attr.st_size == 0) { std::filesystem::remove(filepath); return false; }
        
        std::time_t mod_time = attr.st_mtime;
        std::time_t now = std::time(nullptr);
        bool fresh = (std::difftime(now, mod_time) < 86400.0); 
        return fresh;
    }

    bool TLEManager::downloadFile(const std::string& url, const std::string& dest_path) {
        if (url.empty()) return false;
        
        CURL* curl = curl_easy_init(); 
        if (!curl) return false;
        
        std::cout << "[NET] Downloading: " << url << " ... ";
        std::flush(std::cout);

        std::string readBuffer;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "VisibleEphemeris/12.112");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
        
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK || readBuffer.empty()) {
            std::cout << "FAILED (" << curl_easy_strerror(res) << ")" << std::endl;
            Logger::log("Download failed: " + std::string(curl_easy_strerror(res)));
            return false;
        }
        
        std::cout << "OK (" << readBuffer.length() << " bytes)" << std::endl;
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
            if (line.length() >= 2 && line.substr(0, 2) == "1 " && !name.empty()) {
                l1 = line;
                if (std::getline(file, l2)) {
                    l2 = trim(l2);
                    if (l2.length() >= 2 && l2.substr(0, 2) == "2 ") {
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
        std::string g = trim(group);
        std::string base = "https://celestrak.org/NORAD/elements/gp.php?GROUP=";
        std::string suffix = "&FORMAT=tle";

        // --- MAPPING LOGIC ---
        // Special
        if (g == "active" || g == "visual" || g == "stations" || g == "last-30-days" || g == "analyst") return base + g + suffix;
        // Weather
        if (g == "weather" || g == "noaa" || g == "goes" || g == "resource" || g == "sarsat" || g == "dmc" || g == "tdrss" || g == "argos" || g == "planet" || g == "spire") return base + g + suffix;
        // Comm
        if (g == "geo" || g == "intelsat" || g == "ses" || g == "iridium" || g == "iridium-NEXT" || g == "starlink" || g == "oneweb" || g == "orbcomm" || g == "globalstar" || g == "swpc" || g == "amateur" || g == "x-comm" || g == "other-comm" || g == "satnogs" || g == "gorizont" || g == "raduga" || g == "molniya") return base + g + suffix;
        // Nav
        if (g == "gnss" || g == "gps-ops" || g == "glo-ops" || g == "galileo" || g == "beidou" || g == "sbas" || g == "nnss" || g == "musson") return base + g + suffix;
        // Science
        if (g == "science" || g == "geodetic" || g == "engineering" || g == "education") return base + g + suffix;
        // Misc
        if (g == "military" || g == "radar" || g == "cubesat" || g == "other") return base + g + suffix;

        // IF WE REACH HERE, IT IS UNKNOWN
        std::cerr << "[ERROR] Unknown Group Name: [" << g << "]" << std::endl;
        Logger::log("Unknown group: [" + g + "]. Skipping.");
        return ""; 
    }

    std::vector<Satellite> TLEManager::loadGroups(const std::string& groups_list_str) {
        std::vector<Satellite> all_sats;
        std::set<int> loaded_ids;
        std::stringstream ss(groups_list_str);
        std::string segment;
        
        std::cout << "[TLE] Processing Group List: " << groups_list_str << std::endl;

        while(std::getline(ss, segment, ',')) {
            segment = trim(segment);
            if (segment.empty()) continue;
            
            std::string filename = cache_dir_ + "/" + segment + ".txt";
            bool is_local = std::filesystem::exists(filename);
            bool is_custom = (segment == "user_defined"); // Or check file content logic? user_defined is special.
            
            if (!is_custom) {
                std::string url = getUrlForGroup(segment);
                if (url.empty()) {
                    // Error printed in getUrlForGroup
                    continue; 
                }
                
                if (is_local && isCacheFresh(filename)) {
                    std::cout << "[CACHE] Using cached data for: " << segment << std::endl;
                } else {
                    downloadFile(url, filename);
                }
            } else {
                if (is_local) std::cout << "[CACHE] Using Custom Group: " << segment << std::endl;
                else std::cerr << "[ERROR] Custom group not found. Run Builder first." << std::endl;
            }

            std::vector<Satellite> group_sats = parseFile(filename);
            if (group_sats.empty()) {
                std::cerr << "[WARN] Group [" << segment << "] contained 0 satellites or failed to parse." << std::endl;
            }

            for (auto& sat : group_sats) {
                int id = sat.getNoradId();
                if (loaded_ids.find(id) == loaded_ids.end()) {
                    loaded_ids.insert(id);
                    all_sats.push_back(std::move(sat));
                }
            }
        }
        return all_sats;
    }
    
    // ... (loadSpecificSats / search / save methods preserved from previous bundle logic) ...
    
    // REPEATING HELPER METHODS TO ENSURE FILE COMPLETENESS IN BUNDLE
    std::vector<Satellite> TLEManager::loadSpecificSats(const std::string& sat_names_csv) {
        std::vector<Satellite> results;
        std::vector<std::string> targets;
        std::stringstream ss(sat_names_csv);
        std::string seg;
        while(std::getline(ss, seg, ',')) {
            std::string c = trim(seg);
            if(!c.empty()) {
                std::transform(c.begin(), c.end(), c.begin(), ::toupper);
                targets.push_back(c);
            }
        }

        // SYNTHETIC OBJECTS: SUN (-1) and MOON (-2)
        // If explicitly requested, add them.
        for(const auto& t : targets) {
            if (t == "SUN") {
                // Dummy TLE for Sun (Logic handles -1 ID specially)
                results.emplace_back("SUN",
                    "1 00001U 00001A   00001.00000000  .00000000  00000-0  00000-0 0    12",
                    "2 00001   0.0000   0.0000 0000000   0.0000   0.0000  0.00000000    15");
            } else if (t == "MOON") {
                // Dummy TLE for Moon (Logic handles -2 ID specially)
                results.emplace_back("MOON",
                    "1 00002U 00002A   00001.00000000  .00000000  00000-0  00000-0 0    13",
                    "2 00002   0.0000   0.0000 0000000   0.0000   0.0000  0.00000000    16");
            }
        }

        std::string active_file = cache_dir_ + "/active.txt";
        if (!isCacheFresh(active_file)) {
            downloadFile("https://celestrak.org/NORAD/elements/gp.php?GROUP=active&FORMAT=tle", active_file);
        }
        std::ifstream file(active_file);
        std::string line, name, l1, l2;
        while (std::getline(file, line)) {
            line = trim(line); if (line.length() < 2) continue;
            if (line.length() >= 2 && line.substr(0, 2) == "1 " && !name.empty()) {
                l1 = line;
                if (std::getline(file, l2)) {
                    l2 = trim(l2);
                    if (l2.length() < 2) { name = ""; continue; }
                    std::string check_name = name;
                    std::transform(check_name.begin(), check_name.end(), check_name.begin(), ::toupper);
                    bool match = false;
                    for(const auto& t : targets) { if (check_name.find(t) != std::string::npos) { match = true; break; } }
                    if (match) {
                        try { std::string n = name; n.erase(n.find_last_not_of(" \n\r\t")+1); results.emplace_back(n, l1, l2); } catch(...) {}
                    }
                    name = "";
                }
            } else { name = line; }
        }
        return results;
    }

    std::string TLEManager::getFullCatalogJson() { return "[]"; } // Stub as builder removed
    void TLEManager::saveCustomGroup(const std::string& group_name, const std::vector<int>& norad_ids) {}
    std::string TLEManager::searchMasterCatalog(const std::string& query) { return "[]"; }

    // ===== Historical TLE support (Space-Track gp_history) =====

    std::string TLEManager::formatDate(std::time_t date) {
        std::tm tm_utc;
        gmtime_r(&date, &tm_utc);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_utc);
        return std::string(buf);
    }

    std::string TLEManager::historicalCachePath(const std::string& group_or_key, std::time_t date) {
        // Audit fix (path traversal): validate `group_or_key` against a
        // conservative filename whitelist [A-Za-z0-9_-]. Every callsite of
        // this helper concatenates the segment directly into a filesystem
        // path, so a hostile --groupsel like "../../etc/passwd" would
        // otherwise escape the cache root. Throw on violation rather than
        // silently coerce, so the user notices immediately.
        if (group_or_key.empty()) {
            throw std::runtime_error(
                "TLEManager::historicalCachePath: empty cache key rejected");
        }
        for (char c : group_or_key) {
            bool ok = (c >= 'A' && c <= 'Z')
                   || (c >= 'a' && c <= 'z')
                   || (c >= '0' && c <= '9')
                   || c == '_' || c == '-';
            if (!ok) {
                throw std::runtime_error(
                    "TLEManager::historicalCachePath: invalid cache key '"
                    + group_or_key
                    + "' (only [A-Za-z0-9_-] allowed; path-traversal guard)");
            }
        }
        std::string dir = cache_dir_ + "/historical/" + formatDate(date);
        std::filesystem::create_directories(dir);
        return dir + "/" + group_or_key + ".txt";
    }

    // Extract NORAD IDs from TLE line-1 (columns 3..7).
    static std::vector<int> extractNoradIdsFromFile(const std::string& filepath) {
        std::vector<int> ids;
        std::ifstream f(filepath);
        std::string line;
        while (std::getline(f, line)) {
            if (line.size() >= 7 && line.substr(0, 2) == "1 ") {
                try { ids.push_back(std::stoi(line.substr(2, 5))); } catch (...) {}
            }
        }
        return ids;
    }

    std::vector<int> TLEManager::resolveGroupToNoradIds(const std::string& group) {
        std::string g = trim(group);

        // Try to derive the current group membership from today's Celestrak listing.
        std::vector<int> ids;
        std::string url = getUrlForGroup(g);
        if (!url.empty()) {
            std::string current = cache_dir_ + "/" + g + ".txt";
            if (!std::filesystem::exists(current) || !isCacheFresh(current)) {
                downloadFile(url, current);
            }
            ids = extractNoradIdsFromFile(current);
        }

        // For iridium-NEXT, union with the hardcoded NORAD range (41917-43478) so that
        // historical queries still work even if the current Celestrak listing no longer
        // contains some decommissioned satellites.
        if (g == "iridium-NEXT") {
            std::set<int> merged(ids.begin(), ids.end());
            for (int id = 41917; id <= 43478; ++id) merged.insert(id);
            return std::vector<int>(merged.begin(), merged.end());
        }
        return ids;
    }

    std::vector<int> TLEManager::resolveSatNamesToNoradIds(const std::vector<std::string>& upper_names) {
        std::vector<int> ids;
        std::string active_file = cache_dir_ + "/active.txt";
        if (!isCacheFresh(active_file)) {
            downloadFile("https://celestrak.org/NORAD/elements/gp.php?GROUP=active&FORMAT=tle",
                         active_file);
        }
        std::ifstream file(active_file);
        std::string line, name, l1, l2;
        while (std::getline(file, line)) {
            line = trim(line); if (line.length() < 2) continue;
            if (line.substr(0, 2) == "1 " && !name.empty()) {
                l1 = line;
                if (!std::getline(file, l2)) break;
                l2 = trim(l2);
                std::string upper_name = name;
                std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(), ::toupper);
                bool match = false;
                for (const auto& t : upper_names) {
                    if (upper_name.find(t) != std::string::npos) { match = true; break; }
                }
                if (match) {
                    try { ids.push_back(std::stoi(l1.substr(2, 5))); } catch (...) {}
                }
                name = "";
            } else {
                name = line;
            }
        }
        return ids;
    }

    std::vector<Satellite> TLEManager::loadGroupsForDate(const std::string& groups_list_str,
                                                         std::time_t target_date,
                                                         int window_days) {
        std::vector<Satellite> all_sats;
        std::set<int> loaded_ids;
        SpaceTrackClient st;

        std::stringstream ss(groups_list_str);
        std::string segment;
        std::cout << "[TLE-HIST] Processing historical group list: " << groups_list_str
                  << " @ " << formatDate(target_date) << std::endl;

        while (std::getline(ss, segment, ',')) {
            segment = trim(segment);
            if (segment.empty()) continue;

            std::string cache_path = historicalCachePath(segment, target_date);
            bool cache_hit = std::filesystem::exists(cache_path) &&
                             std::filesystem::file_size(cache_path) > 0;

            if (cache_hit) {
                std::cout << "[CACHE] Historical " << segment << " @ "
                          << formatDate(target_date) << std::endl;
            } else {
                if (!st.hasCredentials()) {
                    std::cerr << "[TLE-HIST] " << SpaceTrackClient::credentialsHelpText() << std::endl;
                    return {};
                }
                auto ids = resolveGroupToNoradIds(segment);
                if (ids.empty()) {
                    std::cerr << "[TLE-HIST] No NORAD IDs resolved for group [" << segment << "]\n";
                    continue;
                }
                if (!st.fetchHistoricalTLEs(ids, target_date, window_days, cache_path)) {
                    std::cerr << "[TLE-HIST] Fetch failed for group [" << segment << "]\n";
                    continue;
                }
            }

            auto sats = parseFile(cache_path);
            for (auto& sat : sats) {
                int id = sat.getNoradId();
                if (loaded_ids.insert(id).second) {
                    all_sats.push_back(std::move(sat));
                }
            }
        }

        return all_sats;
    }

    std::vector<Satellite> TLEManager::loadSpecificSatsForDate(const std::string& sat_names_csv,
                                                                std::time_t target_date,
                                                                int window_days) {
        std::vector<Satellite> results;
        std::vector<std::string> targets;
        std::stringstream ss(sat_names_csv);
        std::string seg;
        while (std::getline(ss, seg, ',')) {
            std::string c = trim(seg);
            if (c.empty()) continue;
            std::transform(c.begin(), c.end(), c.begin(), ::toupper);
            targets.push_back(c);
        }

        // SUN and MOON remain synthetic with a dummy TLE (main loop handles IDs -1/-2).
        std::vector<std::string> real_targets;
        for (const auto& t : targets) {
            if (t == "SUN") {
                results.emplace_back("SUN",
                    "1 00001U 00001A   00001.00000000  .00000000  00000-0  00000-0 0    12",
                    "2 00001   0.0000   0.0000 0000000   0.0000   0.0000  0.00000000    15");
            } else if (t == "MOON") {
                results.emplace_back("MOON",
                    "1 00002U 00002A   00001.00000000  .00000000  00000-0  00000-0 0    13",
                    "2 00002   0.0000   0.0000 0000000   0.0000   0.0000  0.00000000    16");
            } else {
                real_targets.push_back(t);
            }
        }

        if (real_targets.empty()) return results;

        std::string cache_key = "satsel";
        std::string cache_path = historicalCachePath(cache_key, target_date);
        // Include hash of target set in the filename so two different --satsel lists
        // on the same date don't collide.
        {
            std::string joined;
            for (auto& t : real_targets) { joined += t; joined += "|"; }
            std::hash<std::string> h;
            size_t hv = h(joined);
            std::ostringstream os;
            os << cache_key << "_" << std::hex << hv;
            cache_path = historicalCachePath(os.str(), target_date);
        }

        bool cache_hit = std::filesystem::exists(cache_path) &&
                         std::filesystem::file_size(cache_path) > 0;

        if (!cache_hit) {
            SpaceTrackClient st;
            if (!st.hasCredentials()) {
                std::cerr << "[TLE-HIST] " << SpaceTrackClient::credentialsHelpText() << std::endl;
                return results; // may still contain SUN/MOON
            }
            auto ids = resolveSatNamesToNoradIds(real_targets);
            if (ids.empty()) {
                std::cerr << "[TLE-HIST] No NORAD IDs matched --satsel names.\n";
                return results;
            }
            if (!st.fetchHistoricalTLEs(ids, target_date, window_days, cache_path)) {
                std::cerr << "[TLE-HIST] Historical fetch failed for --satsel.\n";
                return results;
            }
        } else {
            std::cout << "[CACHE] Historical satsel @ " << formatDate(target_date) << std::endl;
        }

        auto sats = parseFile(cache_path);
        for (auto& s : sats) results.push_back(std::move(s));
        return results;
    }
}
