#include "rotator.hpp"
#include <iostream>

namespace ve {
    Rotator::Rotator(const std::string& host, int port) : host_(host), port_(port) {
        connect();
    }

    Rotator::~Rotator() {
        disconnect();
    }

    void Rotator::connect() {
        rot_set_debug(ROT_DEBUG_NONE);
        rot_init(&rot_);
        if (!rot_) {
            logger_.log(LogLevel::ERROR, "Failed to initialize rotator");
            return;
        }

        strncpy(rot_->prot.path, (host_ + ":" + std::to_string(port_)).c_str(), sizeof(rot_->prot.path) - 1);
        rot_->prot.path[sizeof(rot_->prot.path) - 1] = '\0';

        // Using rig model 2 for rotctld
        if (rot_open(rot_) != RIG_OK) {
            logger_.log(LogLevel::ERROR, "Failed to connect to rotator at " + host_ + ":" + std::to_string(port_));
            rot_cleanup(rot_);
            rot_ = nullptr;
        } else {
            logger_.log(LogLevel::INFO, "Connected to rotator at " + host_ + ":" + std::to_string(port_));
            connected_ = true;
        }
    }

    void Rotator::disconnect() {
        if (connected_) {
            rot_close(rot_);
            rot_cleanup(rot_);
            rot_ = nullptr;
            connected_ = false;
            logger_.log(LogLevel::INFO, "Disconnected from rotator");
        }
    }

    bool Rotator::isConnected() const {
        return connected_;
    }

    bool Rotator::setPosition(double azimuth, double elevation) {
        if (!connected_) {
            logger_.log(LogLevel::WARNING, "Not connected to rotator, cannot set position");
            return false;
        }

        if (rot_set_position(rot_, azimuth, elevation) != RIG_OK) {
            logger_.log(LogLevel::ERROR, "Failed to set rotator position");
            return false;
        }

        return true;
    }
}
