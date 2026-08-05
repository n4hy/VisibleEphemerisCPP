// logger.cpp - Thread-safe append logger implementation.
//
// The log stream is opened lazily on the first log() call so the path
// resolution can consult the environment at runtime (not static init) and
// so a build that never logs pays no filesystem cost.
//
// Audit fix (LOW security): log lines are sanitised of ASCII control
// characters (< 0x20 except tab, and DEL) before being written. Satellite
// names from unauthenticated TLE feeds can carry ANSI escape sequences
// that would rewrite the reader's terminal when the log is `cat`-ed;
// stripping the escapes defuses that class of "log-injection" attack.
//
// Audit fix (LOW hygiene): the log path is no longer hard-coded to
// ve_log.txt in the current working directory. See logger.hpp for the
// resolution order.
#include "logger.hpp"
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

namespace ve {

    std::ofstream Logger::log_file_;
    std::string   Logger::log_path_;
    bool          Logger::init_attempted_ = false;
    std::mutex    Logger::log_mutex_;

    namespace {
        // Try to open `path` for append, creating parents. Returns true on
        // success and leaves `out` set to that ofstream. If parent creation
        // fails or the file cannot be opened, returns false and `out` is
        // left un-opened.
        bool tryOpenAppend(const std::string& path, std::ofstream& out) {
            if (path.empty()) return false;
            std::error_code ec;
            std::filesystem::path p(path);
            if (p.has_parent_path()) {
                std::filesystem::create_directories(p.parent_path(), ec);
                // Ignore ec; open() below will report the real error.
            }
            out.open(path, std::ios::out | std::ios::app);
            return out.is_open();
        }

        // Sanitise a log line: replace ASCII control chars (< 0x20 except
        // \t) and DEL (0x7F) with '?'. Preserves multibyte UTF-8 (>= 0x80).
        std::string sanitiseLine(const std::string& in) {
            std::string out;
            out.reserve(in.size());
            for (unsigned char c : in) {
                if ((c < 0x20 && c != '\t') || c == 0x7F) {
                    out.push_back('?');
                } else {
                    out.push_back(static_cast<char>(c));
                }
            }
            return out;
        }
    }

    void Logger::ensureOpen() {
        if (init_attempted_) return;
        init_attempted_ = true;

        // 1. Explicit override.
        if (const char* env = std::getenv("VE_LOG_FILE")) {
            if (tryOpenAppend(env, log_file_)) { log_path_ = env; return; }
        }
        // 2. XDG_STATE_HOME.
        if (const char* xdg = std::getenv("XDG_STATE_HOME")) {
            std::string p = std::string(xdg) + "/visible-ephemeris/ve.log";
            if (tryOpenAppend(p, log_file_)) { log_path_ = p; return; }
        }
        // 3. $HOME/.local/state.
        if (const char* home = std::getenv("HOME")) {
            std::string p = std::string(home) + "/.local/state/visible-ephemeris/ve.log";
            if (tryOpenAppend(p, log_file_)) { log_path_ = p; return; }
        }
        // 4. CWD fallback for backward compat (original behaviour).
        if (tryOpenAppend("ve_log.txt", log_file_)) { log_path_ = "ve_log.txt"; return; }

        // No writable location -- silently fall through. log() will no-op.
    }

    void Logger::log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        ensureOpen();
        if (!log_file_.is_open()) return;
        std::time_t now = std::time(nullptr);
        std::tm local_tm;
        localtime_r(&now, &local_tm);
        log_file_ << "[" << std::put_time(&local_tm, "%T") << "] "
                  << sanitiseLine(msg) << std::endl;
        log_file_.flush();
    }

    std::string Logger::activePath() {
        std::lock_guard<std::mutex> lock(log_mutex_);
        ensureOpen();
        return log_file_.is_open() ? log_path_ : std::string();
    }
}
