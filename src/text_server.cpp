#include "text_server.hpp"
#include "logger.hpp"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h> 
#include <arpa/inet.h>

namespace ve {
    TextServer::TextServer(int port) : port_(port), server_fd_(-1), running_(false) {
        // BIND IN CONSTRUCTOR TO FAIL FAST
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) throw std::runtime_error("TextServer: Failed to create socket");
        
        int opt = 1; setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        sockaddr_in address; address.sin_family = AF_INET; address.sin_addr.s_addr = INADDR_ANY; address.sin_port = htons(port_);
        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            throw std::runtime_error("TextServer: Failed to bind port " + std::to_string(port_));
        }
        if (listen(server_fd_, 10) < 0) throw std::runtime_error("TextServer: Failed to listen");
        Logger::log("TextServer started on port " + std::to_string(port_));
    }

    TextServer::~TextServer() { stop(); }

    void TextServer::start() {
        running_ = true;
        server_thread_ = std::thread(&TextServer::serverLoop, this);
    }

    void TextServer::stop() {
        running_ = false;
        if (server_fd_ >= 0) { shutdown(server_fd_, SHUT_RDWR); close(server_fd_); server_fd_ = -1; }
        if(server_thread_.joinable()) server_thread_.join();
    }

    void TextServer::updateData(const std::string& text_view) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        current_text_view_ = text_view;
    }

    void TextServer::serverLoop() {
        while (running_) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int new_socket = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
            
            if (new_socket >= 0) {
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
                Logger::log("Text Client connected from " + std::string(client_ip));

                // 1. Set Non-Blocking for Read with Timeout
                struct timeval t_out; t_out.tv_sec = 0; t_out.tv_usec = 200000; // 200ms timeout
                setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t_out, sizeof(t_out));
                
                // 2. Drain Request Buffer
                char buffer[4096]; 
                int r = read(new_socket, buffer, 4096); 

                // 3. PREPARE RESPONSE IMMEDIATELY
                std::string local_view;
                {
                    std::lock_guard<std::mutex> lock(data_mutex_);
                    local_view = current_text_view_;
                }
                
                std::string content = 
                    "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='1'>"
                    "<title>Visible Ephemeris Terminal</title>"
                    "<style>body { background: #000; color: #0f0; font-family: monospace; font-size: 14px; white-space: pre; }</style>"
                    "</head><body>" + local_view + "</body></html>";
                
                // 4. SEND WITH CONTENT-LENGTH
                std::string response = "HTTP/1.0 200 OK\r\n"
                                       "Content-Type: text/html; charset=utf-8\r\n"
                                       "Content-Length: " + std::to_string(content.length()) + "\r\n"
                                       "Connection: close\r\n\r\n" + content;
                
                send(new_socket, response.c_str(), response.length(), MSG_NOSIGNAL);
                Logger::log("Sent " + std::to_string(response.length()) + " bytes to text client");

                // 5. GRACEFUL CLOSE
                shutdown(new_socket, SHUT_RDWR); 
                close(new_socket); 
            } else { 
                if(!running_) break; 
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); 
            }
        }
    }
}
