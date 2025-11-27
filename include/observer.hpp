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
