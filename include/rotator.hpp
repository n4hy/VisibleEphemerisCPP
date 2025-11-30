#pragma once

#include <string>
#include <hamlib/rotator.h>
#include "logger.hpp"

namespace ve {
    class Rotator {
    public:
        Rotator(const std::string& host, int port);
        ~Rotator();

        bool isConnected() const;
        bool setPosition(double azimuth, double elevation);

    private:
        void connect();
        void disconnect();

        std::string host_;
        int port_;
        ROT* rot_{nullptr};
        bool connected_{false};
    };
}
