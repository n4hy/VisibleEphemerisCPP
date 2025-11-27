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
