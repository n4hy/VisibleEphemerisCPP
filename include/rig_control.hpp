#pragma once
#include <string>
#include <hamlib/rig.h>
#include "logger.hpp"

namespace ve {
    class RigControl {
    public:
        RigControl(const std::string& host, int port);
        ~RigControl();

        bool isConnected() const;
        void setFrequencies(double uplink, double downlink);

    private:
        void connect();
        void disconnect();

        std::string host_;
        int port_;
        RIG* rig_{nullptr};
        bool connected_{false};
    };
}
