#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include "display.hpp"
#include "satellite.hpp"
#include "config_manager.hpp" 
#include "visibility.hpp"
#include "tle_manager.hpp"

namespace ve {
    class WebServer {
    public:
        // builder_mode: true = Mission Planner, false = Dashboard
        WebServer(int port, TLEManager& tle_mgr, bool builder_mode);
        ~WebServer();
        
        void start(); // Non-blocking (for Tracker)
        void runBlocking(); // Blocking (for Builder Phase)
        void stop();

        void updateData(const std::vector<DisplayRow>& rows, const std::vector<Satellite*>& raw_sats, const AppConfig& config, const TimePoint& t, const std::string& time_str);
        
        bool hasPendingConfig();
        AppConfig popPendingConfig();

        int getSelectedNoradId() const;

    private:
        int port_;
        int server_fd_;
        bool builder_mode_;
        std::atomic<bool> running_;
        std::thread server_thread_;
        std::atomic<int> selected_norad_id_{0};
        
        std::mutex data_mutex_;
        std::string current_json_data_;
        AppConfig last_known_config_; 
        
        TLEManager& tle_mgr_;

        std::mutex config_mutex_;
        AppConfig pending_config_;
        bool config_changed_ = false;

        void serverLoop();
        std::string buildJson(const std::vector<DisplayRow>& rows, const std::vector<Satellite*>& raw_sats, const AppConfig& config, const TimePoint& t, const std::string& time_str);
        void handleRequest(int client_socket, const std::string& request);
        std::map<std::string, std::string> parseQuery(const std::string& query);
        std::string urlDecode(const std::string& str);
    };
}
