#pragma once
#include <string>
#include <vector>
#include <map>
#include "satellite.hpp"

namespace ve {
    class TLEManager {
    public:
        TLEManager(const std::string& cache_dir);
        
        // Load tracking groups
        std::vector<Satellite> loadGroups(const std::string& groups_list_str);
        
        // Load specific sats for tracking (from config)
        std::vector<Satellite> loadSpecificSats(const std::string& sat_names_csv);
        
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
    };
}
