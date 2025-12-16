#pragma once

#include <string>

#ifdef ENABLE_HAMLIB
#include <hamlib/rotator.h>
#endif

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
        bool connected_{false};

#ifdef ENABLE_HAMLIB
        ROT* rot_{nullptr};
#endif
    };
}
