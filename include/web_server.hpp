#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include "display.hpp"

namespace ve {
    class WebServer {
    public:
        WebServer(int port);
        ~WebServer();
        void start();
        void stop();
        void updateData(const std::vector<DisplayRow>& rows);

    private:
        int port_;
        int server_fd_;
        std::atomic<bool> running_;
        std::thread server_thread_;
        std::mutex data_mutex_;
        std::string current_json_data_;

        void serverLoop();
        std::string buildJson(const std::vector<DisplayRow>& rows);
    };
}
