#include "rig_control.hpp"
#include <iostream>
#include <algorithm>

namespace ve {
    RigControl::RigControl(const std::string& host, int port) : host_(host), port_(port) {
        connect();
    }

    RigControl::~RigControl() {
        disconnect();
    }

    void RigControl::connect() {
        rig_set_debug(RIG_DEBUG_NONE);
        rig_ = rig_init(2); // 2 = NET rigctl
        if (!rig_) {
            Logger::log("ERROR: RigControl: Failed to initialize rig");
            return;
        }

        std::string rig_pathname = host_ + ":" + std::to_string(port_);
        if (rig_set_conf(rig_, (token_t)"rig_pathname", rig_pathname.c_str()) != RIG_OK) {
            Logger::log("ERROR: RigControl: Failed to set rig pathname");
            rig_cleanup(rig_);
            rig_ = nullptr;
            return;
        }

        if (rig_open(rig_) != RIG_OK) {
            Logger::log("ERROR: RigControl: Failed to connect to rig at " + host_ + ":" + std::to_string(port_));
            rig_cleanup(rig_);
            rig_ = nullptr;
        } else {
            Logger::log("INFO: RigControl: Connected to rig at " + host_ + ":" + std::to_string(port_));
            connected_ = true;
        }
    }

    void RigControl::disconnect() {
        if (connected_) {
            rig_close(rig_);
            rig_cleanup(rig_);
            rig_ = nullptr;
            connected_ = false;
            Logger::log("INFO: RigControl: Disconnected");
        }
    }

    bool RigControl::isConnected() const {
        return connected_;
    }

    void RigControl::setFrequencies(double uplink, double downlink) {
        if (!connected_) return;

        // VFO A / Main = Downlink (Rx)
        if (rig_set_freq(rig_, RIG_VFO_A, static_cast<freq_t>(downlink)) != RIG_OK) {
             rig_set_freq(rig_, RIG_VFO_MAIN, static_cast<freq_t>(downlink));
        }

        // VFO B / Sub = Uplink (Tx)
        if (uplink > 0) {
            if (rig_set_freq(rig_, RIG_VFO_B, static_cast<freq_t>(uplink)) != RIG_OK) {
                 rig_set_freq(rig_, RIG_VFO_SUB, static_cast<freq_t>(uplink));
            }
        }
    }

    void RigControl::setMode(const std::string& mode_str) {
        if (!connected_ || mode_str.empty()) return;

        rmode_t mode = RIG_MODE_FM; // Default
        pbwidth_t width = RIG_PASSBAND_NORMAL;

        std::string m = mode_str;
        std::transform(m.begin(), m.end(), m.begin(), ::toupper);

        if (m.find("FM") != std::string::npos) mode = RIG_MODE_FM;
        else if (m.find("SSB") != std::string::npos || m.find("USB") != std::string::npos || m.find("LSB") != std::string::npos) {
            mode = RIG_MODE_USB; // Standard for sats is usually USB for uplink/downlink on V/U?
            // Actually, Linear transponders usually invert: Uplink LSB/USB -> Downlink USB/LSB.
            // But tracking software usually sets VFO modes.
            // SatNOGS usually specifies "SSB" or "USB"/"LSB".
            // If just "SSB", assume USB for now as safer default for V/U.
            if (m.find("LSB") != std::string::npos) mode = RIG_MODE_LSB;
        }
        else if (m.find("CW") != std::string::npos) mode = RIG_MODE_CW;
        else if (m.find("AM") != std::string::npos) mode = RIG_MODE_AM;

        // Set mode on VFO A (Rx)
        rig_set_mode(rig_, RIG_VFO_A, mode, width);
        // And VFO B? Usually Tx follows or is set separately.
        // For simple Doppler, setting Rx mode is priority.
    }
}
