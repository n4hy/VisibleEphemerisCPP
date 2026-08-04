#pragma once
#include "types.hpp"

// Low-precision analytic Sun and Moon ephemerides (Montenbruck & Gill,
// "Satellite Orbits", Sec. 3.3.2). Two frame conventions are provided:
//
//   sunPositionECI / moonPositionECI   -- J2000 mean equator / mean equinox.
//   sunPositionTEME / moonPositionTEME -- rotated into the mean equator /
//                                         mean equinox of date (MOD), the
//                                         frame the rest of the propagator
//                                         and observer chain uses. MOD
//                                         differs from true TEME only by
//                                         the equation of the equinoxes
//                                         (~arcsec), negligible for
//                                         third-body perturbations.
//
// The J2000-labelled functions are the raw Montenbruck-Gill output and are
// preserved for external callers (e.g. Python bindings). The TEME functions
// are what internal force / SRP code must use so that the third-body vectors
// live in the same frame as the state being integrated.
//
// Accuracy: Sun ~0.1-1 arcmin, Moon ~few arcmin / few hundred km. This is far
// more than sufficient for third-body perturbations and SRP shadow geometry,
// where the perturbing acceleration is many orders of magnitude below the
// central term. The time argument is a Julian Date; UTC is used in place of
// Terrestrial Time (the ~69 s difference is negligible here).
namespace ve {
    // Gravitational parameters of the third bodies (km^3/s^2).
    constexpr double GM_SUN  = 1.32712440018e11;
    constexpr double GM_MOON = 4902.800066;
    constexpr double ASTRONOMICAL_UNIT_KM = 149597870.7;
    constexpr double SOLAR_PRESSURE_1AU = 4.560e-6; // N/m^2 at 1 AU

    // Raw Montenbruck-Gill outputs -- J2000 mean equator, mean equinox.
    Vector3 sunPositionECI(double jd);   // km, J2000
    Vector3 moonPositionECI(double jd);  // km, J2000

    // Apply the IAU-1976 (Lieske 1977) precession rotation J2000 -> MOD
    // (mean equator and mean equinox of date). Returns the rotated vector.
    // Sufficient for third-body use where the difference between MOD and
    // true TEME (~arcsec) contributes < 1 km position error at 1 AU and is
    // negligible for the perturbing acceleration.
    Vector3 precessJ2000ToMOD(double jd, const Vector3& v_j2000);

    // Convenience: Sun / Moon position already rotated into the MOD frame
    // (the propagator's TEME-as-pseudo-inertial working frame).
    inline Vector3 sunPositionTEME(double jd)  { return precessJ2000ToMOD(jd, sunPositionECI(jd)); }
    inline Vector3 moonPositionTEME(double jd) { return precessJ2000ToMOD(jd, moonPositionECI(jd)); }
}
