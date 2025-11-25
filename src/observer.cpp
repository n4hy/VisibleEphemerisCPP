#include "observer.hpp"
#include <cmath>

namespace ve {
    Observer::Observer(double lat, double lon, double alt) : location_{lat, lon, alt} {}

    double Observer::getGST(const TimePoint& t) const {
        double jd = toJulianDate(t);
        double jd_midnight = std::floor(jd - 0.5) + 0.5;
        double T = (jd_midnight - 2451545.0) / 36525.0;
        double gmst_0h = 24110.54841 + 8640184.812866 * T + 0.093104 * T * T - 6.2e-6 * T * T * T;
        double ut_hours = (jd - jd_midnight) * 24.0;
        double gmst_now_sec = gmst_0h + (ut_hours * 3600.0 * 1.00273790935);
        gmst_now_sec = std::fmod(gmst_now_sec, 86400.0);
        if (gmst_now_sec < 0) gmst_now_sec += 86400.0;
        return gmst_now_sec * (2.0 * PI / 86400.0);
    }

    Vector3 Observer::getPositionECI(const TimePoint& t) const {
        double lat_rad = location_.lat_deg * DEG2RAD; 
        double lon_rad = location_.lon_deg * DEG2RAD;
        double a = 6378.137; double f = 1.0 / 298.257223563; double e2 = 2*f - f*f;
        double N = a / std::sqrt(1 - e2 * std::sin(lat_rad) * std::sin(lat_rad));
        double x_ecf = (N + location_.alt_km) * std::cos(lat_rad) * std::cos(lon_rad);
        double y_ecf = (N + location_.alt_km) * std::cos(lat_rad) * std::sin(lon_rad);
        double z_ecf = (N * (1 - e2) + location_.alt_km) * std::sin(lat_rad);
        double theta = getGST(t);
        return { x_ecf * std::cos(theta) - y_ecf * std::sin(theta),
                 x_ecf * std::sin(theta) + y_ecf * std::cos(theta), z_ecf };
    }

    Observer::LookAngle Observer::calculateLookAngle(const Vector3& sat_eci, const TimePoint& t) const {
        Vector3 obs_eci = getPositionECI(t); Vector3 r = sat_eci - obs_eci;
        double lat = location_.lat_deg * DEG2RAD; double lon = location_.lon_deg * DEG2RAD; 
        double th = getGST(t); double lst = th + lon;
        double sL = std::sin(lat); double cL = std::cos(lat); 
        double sLS = std::sin(lst); double cLS = std::cos(lst);
        double s = sL*cLS*r.x + sL*sLS*r.y - cL*r.z;
        double e = -sLS*r.x + cLS*r.y;
        double z = cL*cLS*r.x + cL*sLS*r.y + sL*r.z;
        double range = std::sqrt(s*s + e*e + z*z);
        double az = std::atan2(e, -s); 
        if (az < 0) az += 2*PI;
        return {az * RAD2DEG, std::asin(z/range) * RAD2DEG, range};
    }
}
