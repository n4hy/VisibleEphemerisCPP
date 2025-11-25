#pragma once
#include "types.hpp"

namespace ve {
    class Observer {
    public:
        Observer(double lat, double lon, double alt_km);
        Vector3 getPositionECI(const TimePoint& t) const;
        struct LookAngle { double azimuth; double elevation; double range; };
        LookAngle calculateLookAngle(const Vector3& satellite_eci, const TimePoint& t) const;
        Geodetic getLocation() const { return location_; }
    private:
        Geodetic location_;
        double getGST(const TimePoint& t) const;
    };
}
