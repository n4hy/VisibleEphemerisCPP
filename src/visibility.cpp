#include "visibility.hpp"
#include <cmath>
#include <iostream>

namespace ve {
    Vector3 VisibilityCalculator::getSunPositionECI(const TimePoint& t) {
        double n = toJulianDate(t) - 2451545.0; 
        double L = std::fmod(280.460 + 0.9856474 * n, 360.0); if (L<0) L+=360;
        double g = std::fmod(357.528 + 0.9856003 * n, 360.0); if (g<0) g+=360;
        double lam = (L + 1.915 * std::sin(g*DEG2RAD) + 0.020 * std::sin(2*g*DEG2RAD)) * DEG2RAD;
        double eps = (23.439 - 0.0000004 * n) * DEG2RAD;
        double R = 149597870.7; 
        return {R*std::cos(lam), R*std::cos(eps)*std::sin(lam), R*std::sin(eps)*std::sin(lam)};
    }

    Geodetic VisibilityCalculator::getSunPositionGeo(const TimePoint& t) {
        Vector3 sun_eci = getSunPositionECI(t);
        double gmst = getGMST(t);
        
        // Convert ECI to ECF (Rotate by -GMST)
        double x_ecf = sun_eci.x * std::cos(gmst) + sun_eci.y * std::sin(gmst);
        double y_ecf = -sun_eci.x * std::sin(gmst) + sun_eci.y * std::cos(gmst);
        double z_ecf = sun_eci.z;

        double lon = std::atan2(y_ecf, x_ecf) * RAD2DEG;
        double hyp = std::sqrt(x_ecf*x_ecf + y_ecf*y_ecf);
        double lat = std::atan2(z_ecf, hyp) * RAD2DEG;

        return {lat, lon, 0.0};
    }

    VisibilityCalculator::State VisibilityCalculator::calculateState(const Vector3& sat, const Vector3& obs, const TimePoint& t, double el) {
        Vector3 sun = getSunPositionECI(t);
        double umbra = std::asin(EARTH_RADIUS_KM / sat.magnitude());
        double angle = std::acos(sat.normalize().dot(sun.normalize()));
        bool lit = (angle < (PI/2.0)) || ((PI - angle) >= umbra);
        if (!lit) return State::ECLIPSED;
        double sun_el = (PI/2.0) - std::acos(obs.normalize().dot(sun.normalize()));
        if (sun_el < (-6.0 * DEG2RAD)) return State::VISIBLE;
        return State::DAYLIGHT;
    }

    int VisibilityCalculator::checkFlare(const Vector3& sat_eci, const Vector3& obs_eci, const Vector3& sun_eci, double apogee_km) {
        // 1. Check LEO (<1000 km)
        if (apogee_km > 1000.0) return 0;

        // 2. Check Observer Twilight (Sun Elevation < -12 deg)
        // Angle between Obs and Sun
        double angle_obs_sun = std::acos(obs_eci.normalize().dot(sun_eci.normalize()));
        // Elevation = 90 - Angle
        double sun_el_obs = (PI / 2.0) - angle_obs_sun;
        if (sun_el_obs >= (-12.0 * DEG2RAD)) return 0; // Not dark enough

        // 3. Mirror Geometry
        // Normal points to Earth Center: N = -sat_eci.normalized()
        Vector3 N = sat_eci.normalize() * -1.0;

        // Incident Light Vector (Sun to Sat): I = (Sat - Sun).normalized()
        Vector3 I = (sat_eci - sun_eci).normalize();

        // Check if light hits the Nadir-facing surface
        // Condition: I . N < 0 (Opposing vectors)
        if (I.dot(N) >= 0) return 0; // Light hitting Zenith side

        // Reflection Vector: R = I - 2(I . N)N
        double dot_IN = I.dot(N);
        Vector3 R = I - (N * (2.0 * dot_IN));

        // Vector to Observer: V = (Obs - Sat).normalized()
        Vector3 V = (obs_eci - sat_eci).normalize();

        // Check Angle between Reflection and Observer
        double dot_RV = R.normalize().dot(V);
        if (dot_RV > 1.0) dot_RV = 1.0; // Clamp
        if (dot_RV < -1.0) dot_RV = -1.0;

        double angle_diff_rad = std::acos(dot_RV);
        double angle_diff_deg = angle_diff_rad * RAD2DEG;

        if (angle_diff_deg < 0.5) return 2; // HIT
        if (angle_diff_deg < 1.0) return 1; // NEAR

        return 0;
    }
}
