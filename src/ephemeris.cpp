// ephemeris.cpp - Low-precision analytic Sun and Moon ephemerides.
//
// Implements the truncated trigonometric series of Montenbruck & Gill,
// "Satellite Orbits" (Sec. 3.3.2). Both bodies are returned as geocentric
// position vectors in the J2000 mean-equator/equinox frame, in kilometres.
// These positions feed the third-body gravity terms and the SRP shadow test
// in force_model.cpp; their accuracy (Sun ~arcmin, Moon ~few arcmin) is far
// finer than those perturbations require. See ephemeris.hpp for the interface
// and the gravitational/pressure constants.
#include "ephemeris.hpp"
#include <cmath>

namespace ve {
    namespace {
        constexpr double TWO_PI = 2.0 * PI;
        constexpr double ARCSEC2RAD = PI / (180.0 * 3600.0);
        // J2000 mean obliquity of the ecliptic.
        const double EPS = 23.43929111 * DEG2RAD;

        inline double frac(double x) { return x - std::floor(x); }

        // Rotate an ecliptic-frame vector into the equatorial frame: R_x(-eps).
        inline Vector3 eclipticToEquatorial(double x, double y, double z) {
            double ce = std::cos(EPS), se = std::sin(EPS);
            return { x, y * ce - z * se, y * se + z * ce };
        }
    }

    Vector3 sunPositionECI(double jd) {
        double T = (jd - 2451545.0) / 36525.0;
        // Mean anomaly and ecliptic longitude (radians).
        double M = TWO_PI * frac(0.9931267 + 99.9973583 * T);
        double L = TWO_PI * frac(0.7859444 + M / TWO_PI +
                                 (6892.0 * std::sin(M) + 72.0 * std::sin(2.0 * M)) / 1296.0e3);
        // Geocentric distance (km).
        double r = (149.619e6 - 2.499e6 * std::cos(M) - 0.021e6 * std::cos(2.0 * M));
        // Ecliptic position has zero latitude.
        return eclipticToEquatorial(r * std::cos(L), r * std::sin(L), 0.0);
    }

    Vector3 moonPositionECI(double jd) {
        double T = (jd - 2451545.0) / 36525.0;
        // Fundamental arguments (radians where noted).
        double L0 = frac(0.606433 + 1336.851344 * T);              // mean longitude (rev)
        double l  = TWO_PI * frac(0.374897 + 1325.552410 * T);     // Moon mean anomaly
        double lp = TWO_PI * frac(0.993133 + 99.997361 * T);       // Sun mean anomaly
        double D  = TWO_PI * frac(0.827361 + 1236.853086 * T);     // mean elongation
        double F  = TWO_PI * frac(0.259086 + 1342.227825 * T);     // mean argument of latitude

        // Ecliptic longitude correction (arcsec).
        double dL = 22640.0 * std::sin(l)
                  +   769.0 * std::sin(2.0 * l)
                  -  4586.0 * std::sin(l - 2.0 * D)
                  +  2370.0 * std::sin(2.0 * D)
                  -   668.0 * std::sin(lp)
                  -   412.0 * std::sin(2.0 * F)
                  -   212.0 * std::sin(2.0 * l - 2.0 * D)
                  -   206.0 * std::sin(l + lp - 2.0 * D)
                  +   192.0 * std::sin(l + 2.0 * D)
                  -   165.0 * std::sin(lp - 2.0 * D)
                  +   148.0 * std::sin(l - lp)
                  -   125.0 * std::sin(D)
                  -   110.0 * std::sin(l + lp)
                  -    55.0 * std::sin(2.0 * F - 2.0 * D);
        double lambda = TWO_PI * (L0 + dL / 1296.0e3); // ecliptic longitude (rad)

        // Ecliptic latitude (arcsec -> rad).
        double S = F + (dL + 412.0 * std::sin(2.0 * F) + 541.0 * std::sin(lp)) * ARCSEC2RAD;
        double h = F - 2.0 * D;
        double N = -526.0 * std::sin(h)
                 +   44.0 * std::sin(l + h)
                 -   31.0 * std::sin(-l + h)
                 -   23.0 * std::sin(lp + h)
                 +   11.0 * std::sin(-lp + h)
                 -   25.0 * std::sin(-2.0 * l + F)
                 +   21.0 * std::sin(-l + F);
        double beta = (18520.0 * std::sin(S) + N) * ARCSEC2RAD; // ecliptic latitude (rad)

        // Geocentric distance (km).
        double r = 385000.0
                 - 20905.0 * std::cos(l)
                 -  3699.0 * std::cos(2.0 * D - l)
                 -  2956.0 * std::cos(2.0 * D)
                 -   570.0 * std::cos(2.0 * l)
                 +   246.0 * std::cos(2.0 * l - 2.0 * D)
                 -   205.0 * std::cos(lp - 2.0 * D)
                 -   171.0 * std::cos(l + 2.0 * D)
                 -   152.0 * std::cos(l + lp - 2.0 * D);

        double cb = std::cos(beta);
        return eclipticToEquatorial(r * cb * std::cos(lambda),
                                    r * cb * std::sin(lambda),
                                    r * std::sin(beta));
    }
}
