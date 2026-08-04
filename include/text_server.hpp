// text_server.hpp - Lightweight HTTP text-mirror server.
//
// Serves the most recent terminal frame (the exact text the ncurses Display
// rendered) over HTTP on a port (default 12345), so the live table can be
// viewed in a browser or fetched by scripts. updateData() swaps in the newest
// frame under a mutex; a single-threaded server loop hands it to each request.
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
        // bind_all: true = bind INADDR_ANY (LAN-visible), false = bind
        //   127.0.0.1 only. Default off; the mirror leaks observer / sat
        //   metadata to any client, so LAN exposure must be opted into.
        TextServer(int port, bool bind_all = false);
        ~TextServer();
        void start();
        void stop();
        void updateData(const std::string& text_view);

    private:
        int port_;
        int server_fd_;
        bool bind_all_;
        std::atomic<bool> running_;
        std::thread server_thread_;
        std::mutex data_mutex_;
        std::string current_text_view_;

        void serverLoop();
    };
}
