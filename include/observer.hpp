// observer.hpp - Ground station geometry.
//
// Holds the observer's WGS84 geodetic location and converts a satellite's ECI
// state into observer-relative quantities: the topocentric look angle
// (azimuth/elevation/range) and the line-of-sight range rate. Internally it
// builds the observer's own ECI position/velocity from its geodetic coordinates
// and the Greenwich sidereal angle (getGST). The HPOP Python backend mirrors
// this exact geometry in python_tracker/hpop.py.
#pragma once
#include "types.hpp"

namespace ve {
    class Observer {
    public:
        struct LookAngle {
            double azimuth;
            double elevation;
            double range;
        };

        Observer(double lat, double lon, double alt);
        Geodetic getLocation() const { return location_; }
        Vector3 getPositionECI(const TimePoint& t) const;
        Vector3 getVelocityECI(const TimePoint& t) const;
        LookAngle calculateLookAngle(const Vector3& sat_eci, const TimePoint& t) const;
        double calculateRangeRate(const Vector3& sat_pos, const Vector3& sat_vel, const TimePoint& t) const;

    private:
        Geodetic location_;
        double getGST(const TimePoint& t) const;
    };
}
