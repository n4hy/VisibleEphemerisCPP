// physics_server.hpp - TCP streaming server for raw physics frames.
//
// Listens on a TCP port (default 12346) and broadcasts the latest full-state
// frame to every connected client. Data is only buffered/sent while at least
// one client is connected (hasClients()), so it is free when unused. Runs an
// accept loop and a broadcast loop on dedicated threads; client and data state
// are mutex-protected. Consumed by external tools (see tools/tracking_client.py).
#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <set>

namespace ve {
    // TCP streaming server for physics data
    // Only sends data when clients are connected
    class PhysicsServer {
    public:
        PhysicsServer(int port);
        ~PhysicsServer();
        void start();
        void stop();

        // Update data - only buffers if clients connected
        void updateData(const std::string& data);

        // Check if any clients connected (for caller optimization)
        bool hasClients() const;

        int getClientCount() const;

    private:
        int port_;
        int server_fd_;
        std::atomic<bool> running_;
        std::thread accept_thread_;
        std::thread broadcast_thread_;

        mutable std::mutex clients_mutex_;
        std::set<int> client_fds_;

        std::mutex data_mutex_;
        std::string current_data_;
        bool data_updated_;

        void acceptLoop();
        void broadcastLoop();
        void removeClient(int fd);
    };
}
