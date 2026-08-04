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
// Time argument: the caller passes a Julian Date measured in UTC (the
// convention used by libsgp4 TLE epochs and by the tracker's julianFromTimePoint
// helper). The Montenbruck-Gill series is defined in Terrestrial Time (TT),
// so the internal implementation adds TT_MINUS_UTC_S / 86400 to the input
// before evaluating the series. This removes a ~2000 km tangential Sun and
// ~70 km tangential Moon position bias in the truncated formulas -- both are
// still negligible in the resulting third-body acceleration (<1e-13 km/s^2)
// but the correction is trivial and eliminates a diagnosable error.
//
// Accuracy: Sun ~0.1-1 arcmin, Moon ~few arcmin / few hundred km. This is far
// more than sufficient for third-body perturbations and SRP shadow geometry,
// where the perturbing acceleration is many orders of magnitude below the
// central term.
namespace ve {
    // Gravitational parameters of the third bodies (km^3/s^2).
    constexpr double GM_SUN  = 1.32712440018e11;
    constexpr double GM_MOON = 4902.800066;
    constexpr double ASTRONOMICAL_UNIT_KM = 149597870.7;
    constexpr double SOLAR_PRESSURE_1AU = 4.560e-6; // N/m^2 at 1 AU

    // TT - UTC = 32.184 s + (accumulated leap seconds). As of 2017 the leap
    // count is 37, giving TT - UTC = 69.184 s; this is treated as a nominal
    // constant here because a few seconds of TT/UT1 slop is entirely
    // absorbed in the arcmin-level Montenbruck-Gill truncation error. If
    // a future leap second is added, updating this constant will refine
    // the internal correction; nothing else needs to change.
    constexpr double TT_MINUS_UTC_S = 69.184;

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
