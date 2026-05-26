// logger.hpp - Minimal thread-safe file logger.
//
// Logger::log() appends a timestamped line to the shared log file (ve_log.txt),
// serialized by a static mutex so any thread may log concurrently. Used for
// diagnostics and non-fatal error reporting throughout the application.
#pragma once
#include <string>
#include <fstream>
#include <mutex>

namespace ve {
    class Logger {
    public:
        static void log(const std::string& msg);
    private:
        static std::ofstream log_file_;
        static std::mutex log_mutex_;
    };
}
