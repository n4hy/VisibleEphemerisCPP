#pragma once
#include "types.hpp"
namespace ve {
    class VisibilityCalculator {
    public:
        enum class State { VISIBLE, DAYLIGHT, ECLIPSED };
        static Vector3 getSunPositionECI(const TimePoint& t);
        // New Helper to get Lat/Lon of Sun
        static Geodetic getSunPositionGeo(const TimePoint& t);
        static State calculateState(const Vector3& sat, const Vector3& obs, const TimePoint& t, double el);
    };
}
