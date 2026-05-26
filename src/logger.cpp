// logger.cpp - Thread-safe append logger implementation.
// The output stream and its mutex are static members opened once at startup;
// every log() call timestamps the message (local time) and flushes immediately
// so the log survives a crash.
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
            std::tm local_tm;
            localtime_r(&now, &local_tm);
            log_file_ << "[" << std::put_time(&local_tm, "%T") << "] " << msg << std::endl;
            log_file_.flush();
        }
    }
}
