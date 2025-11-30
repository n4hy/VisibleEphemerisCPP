#pragma once
#include "config_manager.hpp"
#include "tle_manager.hpp"

namespace ve {
    class Builder {
    public:
        static void run(ConfigManager& cfg_mgr, TLEManager& tle_mgr);
    };
}
