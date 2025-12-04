#include "visibility.hpp"
#include <cmath>

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

    // Simplified implementation of Brown's Lunar Theory (approximate to ~1-2 km)
    Vector3 VisibilityCalculator::getMoonPositionECI(const TimePoint& t) {
        double jd = toJulianDate(t);
        double T = (jd - 2451545.0) / 36525.0;

        // Ecliptic longitude (L')
        double L_prime = 218.3164477 + 481267.88123421 * T - 0.0015786 * T*T + T*T*T/538841.0
                        - T*T*T*T/65194000.0;

        // Mean elongation (D)
        double D = 297.8501921 + 445267.1114034 * T - 0.0018819 * T*T + T*T*T/545868.0
                  - T*T*T*T/113065000.0;

        // Sun's mean anomaly (M)
        double M = 357.5291092 + 35999.05034 * T - 0.0001536 * T*T + T*T*T/24490000.0;

        // Moon's mean anomaly (M')
        double M_prime = 134.9633964 + 477198.8675055 * T + 0.0087414 * T*T + T*T*T/69699.0
                        - T*T*T*T/14712000.0;

        // Moon's argument of latitude (F)
        double F = 93.2720950 + 483202.0175233 * T - 0.0036539 * T*T - T*T*T/3526000.0
                  + T*T*T*T/863310000.0;

        // Normalize angles to 0-360
        auto normalize = [](double deg) {
             deg = std::fmod(deg, 360.0);
             if (deg < 0) deg += 360.0;
             return deg;
        };

        L_prime = normalize(L_prime) * DEG2RAD;
        D = normalize(D) * DEG2RAD;
        M = normalize(M) * DEG2RAD;
        M_prime = normalize(M_prime) * DEG2RAD;
        F = normalize(F) * DEG2RAD;

        // Ecliptic longitude (lambda) terms (Degrees) - Main terms
        double sigma_l = 6.288774 * std::sin(M_prime)
                       + 1.274027 * std::sin(2*D - M_prime)
                       + 0.658314 * std::sin(2*D)
                       + 0.213618 * std::sin(2*M_prime)
                       - 0.185116 * std::sin(M)
                       - 0.114332 * std::sin(2*F);
                       // ... truncated for brevity, these are the largest terms

        // Ecliptic latitude (beta) terms (Degrees)
        double sigma_b = 5.128122 * std::sin(F)
                       + 0.280602 * std::sin(M_prime + F)
                       + 0.277693 * std::sin(M_prime - F)
                       + 0.173237 * std::sin(2*D - F);

        // Distance (r) terms (km) - Base distance ~385000 km
        double sigma_r = -20905.355 * std::cos(M_prime)
                       - 3699.111 * std::cos(2*D - M_prime)
                       - 2955.968 * std::cos(2*D)
                       - 569.925 * std::cos(2*M_prime);

        double lambda = L_prime + (sigma_l * DEG2RAD);
        double beta = (sigma_b * DEG2RAD);
        double r = 385000.56 + sigma_r;

        // Obliquity of ecliptic (epsilon)
        double eps = (23.439291 - 0.0130042 * T) * DEG2RAD;

        // Convert to ECI (Equatorial)
        double x_ecl = r * std::cos(beta) * std::cos(lambda);
        double y_ecl = r * std::cos(beta) * std::sin(lambda);
        double z_ecl = r * std::sin(beta);

        // Rotate by epsilon to get Equatorial
        double x_eq = x_ecl;
        double y_eq = y_ecl * std::cos(eps) - z_ecl * std::sin(eps);
        double z_eq = y_ecl * std::sin(eps) + z_ecl * std::cos(eps);

        return {x_eq, y_eq, z_eq};
    }

    Geodetic VisibilityCalculator::getMoonPositionGeo(const TimePoint& t) {
        Vector3 moon_eci = getMoonPositionECI(t);
        double gmst = getGMST(t);

        // Convert ECI to ECF (Rotate by -GMST)
        double x_ecf = moon_eci.x * std::cos(gmst) + moon_eci.y * std::sin(gmst);
        double y_ecf = -moon_eci.x * std::sin(gmst) + moon_eci.y * std::cos(gmst);
        double z_ecf = moon_eci.z;

        double lon = std::atan2(y_ecf, x_ecf) * RAD2DEG;
        double hyp = std::sqrt(x_ecf*x_ecf + y_ecf*y_ecf);
        double lat = std::atan2(z_ecf, hyp) * RAD2DEG;

        // Simple altitude (ignoring flattening for this quick calc, r - Re)
        double alt = std::sqrt(x_ecf*x_ecf + y_ecf*y_ecf + z_ecf*z_ecf) - EARTH_RADIUS_KM;

        return {lat, lon, alt};
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
}
