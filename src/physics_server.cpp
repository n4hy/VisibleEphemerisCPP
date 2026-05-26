// physics_server.cpp - TCP physics-stream server implementation.
// Two threads: acceptLoop() admits clients (non-blocking listen socket,
// TCP_NODELAY for low latency) into a mutex-guarded set; broadcastLoop() pushes
// each newly-updated frame - terminated by a "---END_FRAME---" marker for easy
// client framing - to every client, pruning any that error on send. Frames are
// only buffered while clients are connected, so the server is idle when unused.
#include "physics_server.hpp"
#include "logger.hpp"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>

namespace ve {
    PhysicsServer::PhysicsServer(int port) : port_(port), server_fd_(-1), running_(false), data_updated_(false) {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) throw std::runtime_error("PhysicsServer: Failed to create socket");

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            close(server_fd_);
            throw std::runtime_error("PhysicsServer: Failed to bind port " + std::to_string(port_));
        }
        if (listen(server_fd_, 10) < 0) {
            close(server_fd_);
            throw std::runtime_error("PhysicsServer: Failed to listen");
        }

        // Set server socket to non-blocking for clean shutdown
        fcntl(server_fd_, F_SETFL, O_NONBLOCK);

        Logger::log("PhysicsServer started on port " + std::to_string(port_));
    }

    PhysicsServer::~PhysicsServer() {
        stop();
    }

    void PhysicsServer::start() {
        running_ = true;
        accept_thread_ = std::thread(&PhysicsServer::acceptLoop, this);
        broadcast_thread_ = std::thread(&PhysicsServer::broadcastLoop, this);
    }

    void PhysicsServer::stop() {
        running_ = false;

        // Close all client connections
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (int fd : client_fds_) {
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }
            client_fds_.clear();
        }

        if (server_fd_ >= 0) {
            shutdown(server_fd_, SHUT_RDWR);
            close(server_fd_);
            server_fd_ = -1;
        }

        if (accept_thread_.joinable()) accept_thread_.join();
        if (broadcast_thread_.joinable()) broadcast_thread_.join();
    }

    bool PhysicsServer::hasClients() const {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return !client_fds_.empty();
    }

    int PhysicsServer::getClientCount() const {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return client_fds_.size();
    }

    void PhysicsServer::updateData(const std::string& data) {
        // Only buffer data if clients are connected
        if (!hasClients()) return;

        std::lock_guard<std::mutex> lock(data_mutex_);
        current_data_ = data;
        data_updated_ = true;
    }

    void PhysicsServer::removeClient(int fd) {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        client_fds_.erase(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    void PhysicsServer::acceptLoop() {
        while (running_) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int new_socket = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);

            if (new_socket >= 0) {
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
                Logger::log("Physics client connected from " + std::string(client_ip));

                // Set TCP_NODELAY for low latency
                int flag = 1;
                setsockopt(new_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                // Set non-blocking
                fcntl(new_socket, F_SETFL, O_NONBLOCK);

                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    client_fds_.insert(new_socket);
                }

                Logger::log("Physics clients: " + std::to_string(getClientCount()));
            } else {
                // Non-blocking accept returned no connection
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    void PhysicsServer::broadcastLoop() {
        while (running_) {
            std::string data_to_send;
            bool should_send = false;

            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                if (data_updated_ && !current_data_.empty()) {
                    // Format: newline-terminated for easy parsing
                    data_to_send = current_data_ + "\n---END_FRAME---\n";
                    data_updated_ = false;
                    should_send = true;
                }
            }

            if (should_send && hasClients()) {
                std::vector<int> dead_clients;

                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    for (int fd : client_fds_) {
                        ssize_t sent = send(fd, data_to_send.c_str(), data_to_send.length(), MSG_NOSIGNAL);
                        if (sent < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                dead_clients.push_back(fd);
                            }
                        }
                    }
                }

                // Remove disconnected clients
                for (int fd : dead_clients) {
                    Logger::log("Physics client disconnected");
                    removeClient(fd);
                    Logger::log("Physics clients: " + std::to_string(getClientCount()));
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}
