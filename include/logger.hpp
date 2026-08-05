// logger.hpp - Minimal thread-safe file logger.
//
// Logger::log() appends a timestamped, control-char-stripped line to the
// shared log file, serialized by a static mutex so any thread may log
// concurrently. The log path is resolved lazily on first use (audit fix):
//
//   1. $VE_LOG_FILE (explicit override, absolute or relative)
//   2. $XDG_STATE_HOME/visible-ephemeris/ve.log
//   3. $HOME/.local/state/visible-ephemeris/ve.log
//   4. ./ve_log.txt   (CWD fallback, preserves the historical behaviour)
//
// If none is writable, log() becomes a no-op (does not throw).
#pragma once
#include <string>
#include <fstream>
#include <mutex>

namespace ve {
    class Logger {
    public:
        static void log(const std::string& msg);
        // Exposed for diagnostics: which path resolution ended up as the
        // active log file, or "" if none is open.
        static std::string activePath();
    private:
        static void ensureOpen();     // must be called with log_mutex_ held
        static std::ofstream log_file_;
        static std::string   log_path_;
        static bool          init_attempted_;
        static std::mutex    log_mutex_;
    };
}
