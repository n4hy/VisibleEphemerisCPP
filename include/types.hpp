// types.hpp - Core value types, physical constants, and time utilities shared
// across the whole tracker.
//
// Contents:
//   * Vector3   - minimal 3-D double vector with dot/cross/normalize.
//   * Geodetic  - latitude/longitude/altitude triple (degrees, km).
//   * TimePoint - alias for std::chrono::system_clock time points (UTC).
//   * Constants - Earth radius/GM, deg<->rad, seconds/day, Earth rotation rate.
//   * toJulianDate()/gmstFromJD()/getGMST() - UTC -> Julian Date and Greenwich
//     Mean Sidereal Time, used for every ECI<->ECEF rotation in the codebase.
// Header-only so these primitives stay inlinable and dependency-free.
#pragma once
#include <cmath>
#include <string>
#include <chrono>
#include <ctime>
#include <deque>

namespace ve {
    using Clock = std::chrono::system_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    struct Vector3 {
        double x, y, z;
        Vector3 operator+(const Vector3& other) const { return {x + other.x, y + other.y, z + other.z}; }
        Vector3 operator-(const Vector3& other) const { return {x - other.x, y - other.y, z - other.z}; }
        Vector3 operator*(double s) const { return {x * s, y * s, z * s}; }
        double dot(const Vector3& other) const { return x * other.x + y * other.y + z * other.z; }
        Vector3 cross(const Vector3& o) const { return {y*o.z - z*o.y, z*o.x - x*o.z, x*o.y - y*o.x}; }
        double magnitude() const { return std::sqrt(x*x + y*y + z*z); }
        Vector3 normalize() const {
            double m = magnitude();
            return (m > 0) ? Vector3{x/m, y/m, z/m} : Vector3{0,0,0};
        }
    };
    inline Vector3 operator*(double s, const Vector3& v) { return {v.x*s, v.y*s, v.z*s}; }

    struct Geodetic { double lat_deg; double lon_deg; double alt_km; };

    constexpr double EARTH_RADIUS_KM = 6378.137;
    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG2RAD = PI / 180.0;
    constexpr double RAD2DEG = 180.0 / PI;
    constexpr double SECONDS_PER_DAY = 86400.0;
    constexpr double EARTH_MU = 398600.4415;  // Earth GM (km^3/s^2); == egm96::GM_KM3_S2, single GM used repo-wide
    constexpr double DECAY_ALTITUDE_KM = 80.0;  // Satellites below this apogee are considered decayed

    inline double toJulianDate(const TimePoint& t) {
        std::time_t tt = Clock::to_time_t(t);
        std::tm gmt;
        gmtime_r(&tt, &gmt);
        int Y = gmt.tm_year + 1900; int M = gmt.tm_mon + 1; int D = gmt.tm_mday;
        if (M <= 2) { Y -= 1; M += 12; }
        int A = Y / 100; int B = 2 - A + (A / 4);
        double jd = std::floor(365.25 * (Y + 4716)) + std::floor(30.6001 * (M + 1)) + D + B - 1524.5;
        double fraction = (gmt.tm_hour + gmt.tm_min/60.0 + gmt.tm_sec/3600.0) / 24.0;
        return jd + fraction;
    }

    // Greenwich Mean Sidereal Time (radians) from a Julian Date (UT1 ~= UTC).
    inline double gmstFromJD(double jd) {
        double jd_midnight = std::floor(jd - 0.5) + 0.5;
        double T = (jd_midnight - 2451545.0) / 36525.0;
        double gmst_0h = 24110.54841 + 8640184.812866 * T + 0.093104 * T * T - 6.2e-6 * T * T * T;
        double ut_hours = (jd - jd_midnight) * 24.0;
        double gmst_now_sec = gmst_0h + (ut_hours * 3600.0 * 1.00273790935);
        gmst_now_sec = std::fmod(gmst_now_sec, 86400.0);
        if (gmst_now_sec < 0) gmst_now_sec += 86400.0;
        return (gmst_now_sec / 240.0) * DEG2RAD;
    }

    // Helper to get GMST for coordinate transforms
    inline double getGMST(const TimePoint& t) { return gmstFromJD(toJulianDate(t)); }

    // Earth rotation rate (rad/s), used for the co-rotating atmosphere in drag.
    constexpr double EARTH_ROTATION_RATE = 7.2921159e-5;
}
