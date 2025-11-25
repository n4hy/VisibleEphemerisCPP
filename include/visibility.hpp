#pragma once
#include "types.hpp"

namespace ve {
    class VisibilityCalculator {
    public:
        enum class State { VISIBLE, DAYLIGHT, ECLIPSED, BELOW_HORIZON };
        static State calculateState(const Vector3& sat_eci, const Vector3& obs_eci, const TimePoint& t, double elevation);
    private:
        static Vector3 getSunPositionECI(const TimePoint& t);
        static bool isSatelliteLit(const Vector3& sat_eci, const Vector3& sun_eci);
        static bool isObserverDark(const Vector3& obs_eci, const Vector3& sun_eci);
    };
}
