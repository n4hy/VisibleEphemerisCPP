#include "logger.hpp"
#include <iostream>
#include <ctime>
#include <iomanip>

namespace ve {
    std::ofstream Logger::log_file_("ve_log.txt", std::ios::out | std::ios::app);
    std::mutex Logger::log_mutex_;

    void Logger::log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_file_.is_open()) {
            std::time_t now = std::time(nullptr);
            log_file_ << "[" << std::put_time(std::localtime(&now), "%T") << "] " << msg << std::endl;
            log_file_.flush();
        }
    }
}
