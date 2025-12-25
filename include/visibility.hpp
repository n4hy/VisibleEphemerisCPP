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

        // Flare Calculation: Returns 0=None, 1=Near (0.5-1.0), 2=Hit (<0.5)
        static int checkFlare(const Vector3& sat_eci, const Vector3& obs_eci, const Vector3& sun_eci, double apogee_km);
    };
}
