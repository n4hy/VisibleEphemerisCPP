#include "rotator.hpp"
#include "logger.hpp"
#include <iostream>

#ifdef ENABLE_HAMLIB
#include <hamlib/rig.h>
#endif

namespace ve {
    Rotator::Rotator(const std::string& host, int port) : host_(host), port_(port) {
        connect();
    }

    Rotator::~Rotator() {
        disconnect();
    }

    void Rotator::connect() {
#ifdef ENABLE_HAMLIB
        rig_set_debug(RIG_DEBUG_NONE);
        rot_ = rot_init(2);
        if (!rot_) {
            ve::Logger::log("ERROR: Rotator: Failed to initialize rotator");
            return;
        }

        std::string rot_pathname = host_ + ":" + std::to_string(port_);
        if (rot_set_conf(rot_, (token_t)"rot_pathname", rot_pathname.c_str()) != RIG_OK) {
            ve::Logger::log("ERROR: Rotator: Failed to set rotator pathname");
            rot_cleanup(rot_);
            rot_ = nullptr;
            return;
        }

        if (rot_open(rot_) != RIG_OK) {
            ve::Logger::log("ERROR: Rotator: Failed to connect to rotator at " + host_ + ":" + std::to_string(port_));
            rot_cleanup(rot_);
            rot_ = nullptr;
        } else {
            ve::Logger::log("INFO: Rotator: Connected to rotator at " + host_ + ":" + std::to_string(port_));
            connected_ = true;
        }
#else
        ve::Logger::log("WARNING: Rotator support disabled in build.");
#endif
    }

    void Rotator::disconnect() {
#ifdef ENABLE_HAMLIB
        if (connected_) {
            rot_close(rot_);
            rot_cleanup(rot_);
            rot_ = nullptr;
            connected_ = false;
            ve::Logger::log("INFO: Rotator: Disconnected from rotator");
        }
#endif
    }

    bool Rotator::isConnected() const {
        return connected_;
    }

    bool Rotator::setPosition(double azimuth, double elevation) {
        if (!connected_) {
            ve::Logger::log("WARNING: Rotator: Not connected to rotator, cannot set position");
            return false;
        }

#ifdef ENABLE_HAMLIB
        if (rot_set_position(rot_, azimuth, elevation) != RIG_OK) {
            ve::Logger::log("ERROR: Rotator: Failed to set rotator position");
            return false;
        }
        return true;
#else
        return false;
#endif
    }
}
