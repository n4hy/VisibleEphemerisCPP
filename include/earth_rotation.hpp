#pragma once
//
// earth_rotation -- Earth-orientation rotations for the visibility /
//                   display pipeline.
//
// SGP4 outputs satellite positions in the TEME frame (True Equator,
// Mean Equinox of date), NOT in GCRS / J2000 inertial. The legacy
// helper ``ve::getGMST`` (in types.hpp) computes the Greenwich Mean
// Sidereal Time and is used in ``visibility.cpp`` to rotate ECI -> ECEF.
// That is a GMST-only approximation: it is correct to the equation of
// the equinoxes (E_e, typically < 1.3 arc-seconds), which matters once
// you want degree-accurate sub-satellite points or coordinate frames
// that interoperate with high-precision ephemeris sources (COMSPOC,
// IERS-tagged SP3, etc.).
//
// This header introduces a pluggable rotation interface with two
// implementations:
//
//   ve::GmstRotation     -- the legacy GMST z-rotation (backward
//                           compatible default).
//   ve::IAU2000Rotation  -- TEME -> ITRS using GMST + Equation of the
//                           Equinoxes (IAU 1982 nutation model, the
//                           standard reduction for SGP4 outputs).
//
// Both expose the same interface so call sites can switch rotators
// without restructuring. The full IAU 2006/2000A precession-nutation +
// polar motion chain (using IERS EOP tables) would be the natural next
// step if sub-meter geodesy is ever required; the C++ project is a
// display / tracking appliance and does not yet need that level of
// precision, so it is deliberately left for a follow-on commit.
//
// Author note: this mirrors the python/fixed_location/earth_rotation.py
// abstraction added to the IPNT project; the Python tracker here
// (python_tracker/satellite.py) already uses Skyfield's IAU 2006/2000A
// chain implicitly via ``.at(t)`` / ``wgs84.subpoint()``.

#include "types.hpp"

namespace ve {

    // Abstract rotation interface.  ``R_eci_to_ecef`` returns the
    // matrix such that  r_ecef = R * r_eci.  We provide both the matrix
    // and a vector convenience overload because callers in this project
    // typically rotate a single 3-vector at a time.
    struct Rotation3 {
        double m[3][3];

        Vector3 operator*(const Vector3& v) const {
            return {
                m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z,
                m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z,
                m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z,
            };
        }

        // Transpose-multiply (R^T * v), i.e. apply the inverse rotation
        // for an orthonormal matrix.
        Vector3 transpose_apply(const Vector3& v) const {
            return {
                m[0][0]*v.x + m[1][0]*v.y + m[2][0]*v.z,
                m[0][1]*v.x + m[1][1]*v.y + m[2][1]*v.z,
                m[0][2]*v.x + m[1][2]*v.y + m[2][2]*v.z,
            };
        }
    };

    class IEarthRotation {
    public:
        virtual ~IEarthRotation() = default;
        virtual Rotation3 R_eci_to_ecef(const TimePoint& t) const = 0;

        // Rotate a vector ECI/TEME -> ECEF.  Convenience helper.
        Vector3 rotate_eci_to_ecef(const Vector3& v, const TimePoint& t) const {
            return R_eci_to_ecef(t) * v;
        }
    };

    // Legacy z-rotation by GMST.  Equivalent to the original
    // visibility.cpp pre-rotation: it treats the SGP4-TEME output as
    // if it were aligned with the Mean Equinox of date and rotates by
    // GMST, ignoring the equation of the equinoxes (max ~1.3 as).
    class GmstRotation : public IEarthRotation {
    public:
        Rotation3 R_eci_to_ecef(const TimePoint& t) const override {
            double gmst = getGMST(t);
            double c = std::cos(gmst), s = std::sin(gmst);
            // r_ecef = Rz(+gmst) * r_eci, with the visibility.cpp sign
            // convention preserved (see the original two-line rotation
            // in src/visibility.cpp::getSunPositionGeo).
            Rotation3 R = {{
                {  c,  s, 0.0 },
                { -s,  c, 0.0 },
                { 0.0, 0.0, 1.0 },
            }};
            return R;
        }
    };

    // Equation of the Equinoxes (IAU 1982 nutation model, low-order),
    // returned in radians.  This is the correction that takes GMST
    // (Mean Sidereal Time) to GAST (Apparent Sidereal Time) and is the
    // proper sidereal angle for rotating TEME -> ITRS, since SGP4
    // outputs are referred to the True Equator and Mean Equinox of
    // date.  The standard short series uses the dominant nutation
    // terms in longitude (delta_psi) and the mean obliquity
    // (epsilon_A).
    //
    // Two entry points: one taking a Julian Date (used by the HPOP
    // gravity path, which already lives in JD), and one taking a
    // TimePoint (kept for the visibility pipeline that predates the
    // JD-based force model). Both share the same underlying math.
    inline double equationOfEquinoxes_IAU1982_jd(double jd) {
        double T = (jd - 2451545.0) / 36525.0;            // Julian centuries TT (approx UTC)

        // Mean obliquity of the ecliptic (IAU 1980), in radians.
        double eps_deg = 23.439291111
                        - 0.0130041667 * T
                        - 1.638889e-7 * T * T
                        + 5.036111e-7 * T * T * T;
        double eps = eps_deg * DEG2RAD;

        // Fundamental arguments (IAU 1980 nutation; degrees).
        // Mean anomaly of the Moon (l), Sun (l'), Moon's argument of
        // latitude (F), elongation of the Moon from the Sun (D), and
        // longitude of the ascending node of the Moon (Omega).
        auto frac = [](double x) {
            x = std::fmod(x, 360.0);
            if (x < 0.0) x += 360.0;
            return x;
        };
        double l       = frac(134.96298139 + 477198.867398 * T);
        double lprime  = frac(357.52772333 +  35999.050340 * T);
        double F       = frac( 93.27191028 + 483202.017538 * T);
        double D       = frac(297.85036306 + 445267.111480 * T);
        double Omega   = frac(125.04452222 -   1934.136261 * T);
        l      *= DEG2RAD; lprime *= DEG2RAD;
        F      *= DEG2RAD; D      *= DEG2RAD; Omega *= DEG2RAD;

        // Dominant nutation in longitude (IAU 1980), arc-seconds.
        // (Coefficients are the five largest terms; the full IAU 1980
        // series has 106 terms, but the cumulative truncation error of
        // the leading five is well below 0.05 arc-second.)
        double dpsi_as =
              (-17.1996 - 0.01742 * T) * std::sin(Omega)
            + (-1.3187  - 0.00016 * T) * std::sin(2.0*(F - D + Omega))
            + (-0.2274  - 0.00002 * T) * std::sin(2.0*(F + Omega))
            + ( 0.2062  + 0.00002 * T) * std::sin(2.0*Omega)
            + ( 0.1426  - 0.00034 * T) * std::sin(lprime);

        // arc-seconds -> radians
        const double AS2RAD = (PI / 180.0) / 3600.0;
        double dpsi = dpsi_as * AS2RAD;

        return dpsi * std::cos(eps);                      // E_e (radians)
    }

    inline double equationOfEquinoxes_IAU1982(const TimePoint& t) {
        return equationOfEquinoxes_IAU1982_jd(toJulianDate(t));
    }

    // Greenwich Apparent Sidereal Time from Julian Date: GMST + E_e.
    // Suitable for TEME -> ITRS rotations against SGP4 output.
    inline double gastFromJD(double jd) {
        return gmstFromJD(jd) + equationOfEquinoxes_IAU1982_jd(jd);
    }

    // TEME -> ITRS rotation: Rz(GAST) where GAST = GMST + E_e.
    // This is the standard reduction for SGP4 output and is what
    // ``visibility.cpp`` should be using when the input frame is TEME.
    class IAU2000Rotation : public IEarthRotation {
    public:
        Rotation3 R_eci_to_ecef(const TimePoint& t) const override {
            double gmst = getGMST(t);
            double ee   = equationOfEquinoxes_IAU1982(t);
            double gast = gmst + ee;
            double c = std::cos(gast), s = std::sin(gast);
            Rotation3 R = {{
                {  c,  s, 0.0 },
                { -s,  c, 0.0 },
                { 0.0, 0.0, 1.0 },
            }};
            return R;
        }
    };

    // Default rotation: legacy GMST, so every existing call site
    // continues to behave the same way until it opts in.
    inline const IEarthRotation& defaultRotation() {
        static const GmstRotation R;
        return R;
    }

    // Accuracy-conscious default for new code paths that need the
    // TEME-aware rotation (SGP4 outputs, COMSPOC alignment, etc.).
    inline const IEarthRotation& iauRotation() {
        static const IAU2000Rotation R;
        return R;
    }
}
