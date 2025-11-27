#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include "display.hpp"
#include "satellite.hpp"
#include "config_manager.hpp" 

namespace ve {
    class WebServer {
    public:
        WebServer(int port);
        ~WebServer();
        void start();
        void stop();
        // Updated to accept Config
        void updateData(const std::vector<DisplayRow>& rows, const std::vector<Satellite*>& raw_sats, const AppConfig& config);

    private:
        int port_;
        int server_fd_;
        std::atomic<bool> running_;
        std::thread server_thread_;
        std::mutex data_mutex_;
        std::string current_json_data_;

        void serverLoop();
        std::string buildJson(const std::vector<DisplayRow>& rows, const std::vector<Satellite*>& raw_sats, const AppConfig& config);
    };
}
