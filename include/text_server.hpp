#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include "display.hpp" 

namespace ve {
    class TextServer {
    public:
        TextServer(int port);
        ~TextServer();
        void start();
        void stop();
        void updateData(const std::string& text_view);

    private:
        int port_;
        int server_fd_;
        std::atomic<bool> running_;
        std::thread server_thread_;
        std::mutex data_mutex_;
        std::string current_text_view_;

        void serverLoop();
    };
}
