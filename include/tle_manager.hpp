#pragma once
#include <string>
#include <vector>
#include <map>
#include "satellite.hpp"

namespace ve {
    class TLEManager {
    public:
        TLEManager(const std::string& cache_dir);
        std::vector<Satellite> loadGroups(const std::string& groups_list_str);
        void clearCache();
    private:
        std::string cache_dir_;
        std::string getUrlForGroup(const std::string& group);
        bool downloadFile(const std::string& url, const std::string& dest_path);
        std::vector<Satellite> parseFile(const std::string& filepath);
        static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
        static std::string trim(const std::string& str);
        bool isCacheFresh(const std::string& filepath);
    };
}
