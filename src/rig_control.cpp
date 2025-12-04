#include "rig_control.hpp"
#include <iostream>

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

        // Strategy: Assuming VFO A is Downlink (Rx) and VFO B is Uplink (Tx)
        // Or Main/Sub. hamlib abstracts this somewhat.
        // We will try setting VFO A (Main) to Downlink
        if (rig_set_freq(rig_, RIG_VFO_A, static_cast<freq_t>(downlink)) != RIG_OK) {
             // Try Main if VFO A fails
             rig_set_freq(rig_, RIG_VFO_MAIN, static_cast<freq_t>(downlink));
        }

        // Set VFO B (Sub/Tx) to Uplink
        // Note: For some radios, we might need split mode.
        if (uplink > 0) {
            // Activate Split if not already? (Assuming rig is in split or user set it up)
            // rig_set_split_freq(rig_, RIG_VFO_A, uplink); // Or VFO B

            // Standard approach for Satellites often involves two VFOs
            if (rig_set_freq(rig_, RIG_VFO_B, static_cast<freq_t>(uplink)) != RIG_OK) {
                 rig_set_freq(rig_, RIG_VFO_SUB, static_cast<freq_t>(uplink));
            }
        }
    }
}
