// rotator.hpp - Antenna rotator control via Hamlib.
//
// Thin RAII wrapper around a Hamlib rotctld connection that points an az/el
// antenna rotator at the tracked satellite. Compiled only when ENABLE_HAMLIB is
// defined; otherwise the methods are no-ops/stubs so the rest of the build is
// unaffected. setPosition() commands the rotator to an azimuth/elevation.
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
