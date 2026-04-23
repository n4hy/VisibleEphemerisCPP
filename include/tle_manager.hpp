#pragma once
#include <string>
#include <vector>
#include <map>
#include <ctime>
#include "satellite.hpp"

namespace ve {
    class TLEManager {
    public:
        TLEManager(const std::string& cache_dir);

        // Load tracking groups
        std::vector<Satellite> loadGroups(const std::string& groups_list_str);

        // Load specific sats for tracking (from config)
        std::vector<Satellite> loadSpecificSats(const std::string& sat_names_csv);

        // Historical variants: fetch TLEs that were current on target_date (UTC).
        // Uses Space-Track.org gp_history endpoint. Results are cached under
        // <cache_dir>/historical/<YYYY-MM-DD>/ and are never expired.
        // Requires Space-Track credentials; see SpaceTrackClient::credentialsHelpText().
        std::vector<Satellite> loadGroupsForDate(const std::string& groups_list_str,
                                                 std::time_t target_date,
                                                 int window_days = 10);
        std::vector<Satellite> loadSpecificSatsForDate(const std::string& sat_names_csv,
                                                       std::time_t target_date,
                                                       int window_days = 10);
        
        // NEW: Server-Side Search (returns JSON string of matches)
        std::string searchMasterCatalog(const std::string& query);
        
        // NEW: Save Custom Group
        void saveCustomGroup(const std::string& group_name, const std::vector<int>& norad_ids);
        
        // NEW: Get Full Catalog for legacy support (optional, but good to have)
        std::string getFullCatalogJson();

        void clearCache();

    private:
        std::string cache_dir_;
        std::vector<Satellite> master_catalog_; // In-Memory Cache
        bool master_loaded_ = false;

        void loadMasterCatalogIfNeeded();
        std::string getUrlForGroup(const std::string& group);
        bool downloadFile(const std::string& url, const std::string& dest_path);
        std::vector<Satellite> parseFile(const std::string& filepath);
        static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
        static std::string trim(const std::string& str);
        bool isCacheFresh(const std::string& filepath);

        // Historical helpers
        std::vector<int> resolveGroupToNoradIds(const std::string& group);
        std::vector<int> resolveSatNamesToNoradIds(const std::vector<std::string>& upper_names);
        std::string historicalCachePath(const std::string& group_or_key, std::time_t date);
        static std::string formatDate(std::time_t date);
    };
}
