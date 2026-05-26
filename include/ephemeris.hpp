#pragma once
#include "types.hpp"

// Low-precision analytic Sun and Moon ephemerides (Montenbruck & Gill,
// "Satellite Orbits", Sec. 3.3.2). Positions are geocentric, in the
// equatorial frame of J2000 (mean equator/equinox), in kilometres.
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

    Vector3 sunPositionECI(double jd);   // km
    Vector3 moonPositionECI(double jd);  // km
}
