#include "spacetrack_client.hpp"
#include "logger.hpp"

#include <curl/curl.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <map>
#include <algorithm>
#include <cctype>
#include <ctime>

namespace ve {

    namespace {
        size_t WriteToString(void* contents, size_t size, size_t nmemb, void* userp) {
            ((std::string*)userp)->append((char*)contents, size * nmemb);
            return size * nmemb;
        }

        std::string trimStr(const std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return "";
            size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        }

        std::string formatIso8601(std::time_t t) {
            std::tm tm_utc;
            gmtime_r(&t, &tm_utc);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
            return std::string(buf);
        }

        std::string iniPath() {
            const char* home = std::getenv("HOME");
            if (!home) return "";
            return std::string(home) + "/.config/visible-ephemeris/spacetrack.ini";
        }

        // Very small INI parser: handles "[section]" headers and "key = value" lines,
        // skipping blanks and '#' / ';' comments.
        std::map<std::string, std::string> parseSpaceTrackIni(const std::string& path) {
            std::map<std::string, std::string> out;
            std::ifstream f(path);
            if (!f.is_open()) return out;
            std::string line, section;
            while (std::getline(f, line)) {
                line = trimStr(line);
                if (line.empty() || line[0] == '#' || line[0] == ';') continue;
                if (line.front() == '[' && line.back() == ']') {
                    section = trimStr(line.substr(1, line.size() - 2));
                    continue;
                }
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string k = trimStr(line.substr(0, eq));
                std::string v = trimStr(line.substr(eq + 1));
                if (section == "spacetrack" || section.empty()) {
                    out[k] = v;
                }
            }
            return out;
        }
    }

    SpaceTrackClient::SpaceTrackClient() {
        loadCredentials();
    }

    void SpaceTrackClient::loadCredentials() {
        if (const char* u = std::getenv("SPACETRACK_USER")) username_ = u;
        if (const char* p = std::getenv("SPACETRACK_PASS")) password_ = p;
        if (!username_.empty() && !password_.empty()) return;

        auto ini = parseSpaceTrackIni(iniPath());
        if (username_.empty()) {
            if (auto it = ini.find("username"); it != ini.end()) username_ = it->second;
        }
        if (password_.empty()) {
            if (auto it = ini.find("password"); it != ini.end()) password_ = it->second;
        }
    }

    std::string SpaceTrackClient::credentialsHelpText() {
        return "Space-Track credentials not found. Set env vars SPACETRACK_USER / "
               "SPACETRACK_PASS, or create ~/.config/visible-ephemeris/spacetrack.ini "
               "with a [spacetrack] section containing username and password. "
               "Register a free account at https://www.space-track.org/auth/createAccount";
    }

    std::string SpaceTrackClient::urlEncode(const std::string& s) {
        CURL* c = curl_easy_init();
        if (!c) return s;
        char* esc = curl_easy_escape(c, s.c_str(), (int)s.size());
        std::string out = esc ? esc : "";
        if (esc) curl_free(esc);
        curl_easy_cleanup(c);
        return out;
    }

    bool SpaceTrackClient::fetchHistoricalTLEs(const std::vector<int>& norad_ids,
                                               std::time_t target_date,
                                               int window_days,
                                               const std::string& dest_path) {
        if (!hasCredentials()) {
            std::cerr << "[SPACETRACK] " << credentialsHelpText() << std::endl;
            Logger::log("Space-Track: missing credentials");
            return false;
        }
        if (norad_ids.empty()) {
            std::cerr << "[SPACETRACK] No NORAD IDs supplied for historical query." << std::endl;
            return false;
        }

        CURL* curl = curl_easy_init();
        if (!curl) return false;

        // Space-Track uses a session cookie; route it through an in-memory cookie jar.
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "VisibleEphemeris/spacetrack");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

        // 1. Login
        {
            std::string login_body = "identity=" + urlEncode(username_) +
                                     "&password=" + urlEncode(password_);
            std::string resp;
            curl_easy_setopt(curl, CURLOPT_URL, "https://www.space-track.org/ajaxauth/login");
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, login_body.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

            std::cout << "[SPACETRACK] Logging in..." << std::endl;
            CURLcode rc = curl_easy_perform(curl);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (rc != CURLE_OK || http_code >= 400) {
                std::cerr << "[SPACETRACK] Login failed (http " << http_code << "): "
                          << curl_easy_strerror(rc) << std::endl;
                Logger::log("Space-Track login failed");
                curl_easy_cleanup(curl);
                return false;
            }
            // Space-Track returns an empty body on success; a failed login returns a JSON
            // error describing credential trouble.
            if (resp.find("\"Login\"") != std::string::npos && resp.find("failed") != std::string::npos) {
                std::cerr << "[SPACETRACK] Authentication rejected: " << resp << std::endl;
                curl_easy_cleanup(curl);
                return false;
            }
        }

        // 2. Build gp_history query
        //    /class/gp_history/NORAD_CAT_ID/<ids>/EPOCH/<D-window>--<D>/orderby/NORAD_CAT_ID,EPOCH desc/format/3le
        std::string id_list;
        for (size_t i = 0; i < norad_ids.size(); ++i) {
            if (i) id_list += ",";
            id_list += std::to_string(norad_ids[i]);
        }

        std::time_t start_t = target_date - (std::time_t)window_days * 86400;
        std::time_t end_t   = target_date; // do not include epochs after the target instant
        std::string start_iso = formatIso8601(start_t);
        std::string end_iso   = formatIso8601(end_t);

        std::string url =
            "https://www.space-track.org/basicspacedata/query/class/gp_history"
            "/NORAD_CAT_ID/" + id_list +
            "/EPOCH/" + start_iso + "--" + end_iso +
            "/orderby/NORAD_CAT_ID,EPOCH%20desc/format/3le";

        std::string body;
        body.reserve(1024 * 64);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

        std::cout << "[SPACETRACK] Querying gp_history for " << norad_ids.size()
                  << " IDs around " << formatIso8601(target_date) << " ..." << std::endl;
        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK || http_code >= 400) {
            std::cerr << "[SPACETRACK] Query failed (http " << http_code << "): "
                      << curl_easy_strerror(rc) << std::endl;
            Logger::log("Space-Track gp_history query failed");
            return false;
        }

        if (body.empty()) {
            std::cerr << "[SPACETRACK] Empty response (no matching TLEs in window)." << std::endl;
            return false;
        }

        // 3. Filter: keep only the FIRST (latest) TLE per NORAD ID from the ordered results.
        //    Input is 3LE: name line / "1 ..." / "2 ..." repeated.
        std::vector<std::string> lines;
        {
            std::istringstream iss(body);
            std::string line;
            while (std::getline(iss, line)) {
                lines.push_back(trimStr(line));
            }
        }

        std::ofstream out(dest_path);
        if (!out.is_open()) {
            std::cerr << "[SPACETRACK] Cannot write cache file: " << dest_path << std::endl;
            return false;
        }

        std::map<int, bool> seen;
        int kept = 0;
        for (size_t i = 0; i + 2 < lines.size(); ) {
            const std::string& name = lines[i];
            const std::string& l1   = lines[i + 1];
            const std::string& l2   = lines[i + 2];
            if (l1.size() < 7 || l1.substr(0, 2) != "1 " || l2.substr(0, 2) != "2 ") {
                i += 1; // resync
                continue;
            }
            int id = 0;
            try { id = std::stoi(l1.substr(2, 5)); } catch (...) { i += 3; continue; }
            if (!seen[id]) {
                seen[id] = true;
                out << name << "\n" << l1 << "\n" << l2 << "\n";
                kept++;
            }
            i += 3;
        }
        out.close();

        std::cout << "[SPACETRACK] Retrieved " << kept << " TLEs (one per satellite)." << std::endl;
        Logger::log("Space-Track: " + std::to_string(kept) + " historical TLEs cached to " + dest_path);
        return kept > 0;
    }
}
