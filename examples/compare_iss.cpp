// compare_iss.cpp - Example: HPOP vs SGP4 for the ISS over a full day.
//
// Propagates the same TLE with both the High-Precision Orbit Propagator
// (EGM96 20x20 gravity + Sun/Moon third-body + atmospheric drag + solar
// radiation pressure, adaptive Fehlberg RK7(8)) and analytic SGP4, then prints
// the position difference. Because both propagators emit TEME, the states are
// differenced directly (no frame conversion) and decomposed into the orbital
// RSW frame: R = radial, S = along-track, W = cross-track. The difference is
// dominated by the along-track term - the signature of two models drifting
// apart in phase while staying on essentially the same plane and shape.
//
// This is a model-difference demonstration, not an accuracy verdict: HPOP is
// seeded from SGP4's mean elements at epoch, so neither result is ground truth.
//
// Build:  cmake .. -DBUILD_EXAMPLES=ON && cmake --build . --target compare_iss
// Run:    LD_LIBRARY_PATH=<libsgp4>/lib ./compare_iss
#include "numerical_propagator.hpp"
#include "egm96_coeffs.hpp"
#include <Tle.h>
#include <SGP4.h>
#include <Eci.h>
#include <cstdio>
#include <cmath>

using namespace ve;
static double mag(const Vector3& v) { return std::sqrt(v.dot(v)); }

int main() {
    // Reference ISS element set (epoch 2024 day 079.07758) - fully reproducible.
    const std::string name = "ISS (ZARYA)";
    const std::string l1 = "1 25544U 98067A   24079.07757601  .00016717  00000-0  30074-3 0  9993";
    const std::string l2 = "2 25544  51.6393 110.7232 0005383  62.8273  29.0235 15.49648268446247";
    libsgp4::Tle tle(name, l1, l2);
    libsgp4::SGP4 sgp4(tle);

    ForceParams fp;
    fp.bstar = tle.BStar();   // full model: EGM96 20x20 + Sun/Moon + drag + SRP
    NumericalPropagator hpop(tle, fp);

    printf("ISS HPOP (EGM96 20x20 + Sun/Moon + drag + SRP, RKF7(8)) vs SGP4\n");
    printf("TLE epoch JD %.5f. Both in TEME; differences in km.\n\n", tle.Epoch().ToJulian());
    printf("%6s | %9s | %9s %9s %9s | %8s\n",
           "min", "|dr| km", "radial", "along", "cross", "|dalt|");
    printf("-------+-----------+----------------------------------+---------\n");

    double max_tot = 0, max_min = 0, sum2 = 0, final_tot = 0;
    int n = 0;
    for (int m = 0; m <= 1440; ++m) {           // one full day, 1-minute samples
        double tsec = m * 60.0;
        auto sv = hpop.stateAtSeconds(tsec);
        Vector3 rh = sv.first, vh = sv.second;
        (void)vh;
        libsgp4::Eci e = sgp4.FindPosition(tle.Epoch().AddSeconds(tsec));
        Vector3 rs{e.Position().x, e.Position().y, e.Position().z};
        Vector3 vs{e.Velocity().x, e.Velocity().y, e.Velocity().z};

        Vector3 dr = rh - rs;
        double tot = mag(dr);
        // Orbital RSW basis built from the SGP4 (reference) state.
        Vector3 R = rs.normalize();
        Vector3 W = rs.cross(vs).normalize();
        Vector3 S = W.cross(R);
        double dR = dr.dot(R), dS = dr.dot(S), dW = dr.dot(W);
        double dalt = mag(rh) - mag(rs);

        if (tot > max_tot) { max_tot = tot; max_min = m; }
        sum2 += tot * tot; ++n; final_tot = tot;

        if (m % 60 == 0)
            printf("%6d | %9.3f | %9.3f %9.3f %9.3f | %8.3f\n", m, tot, dR, dS, dW, dalt);
    }
    printf("\nOver 24 h (1-min samples): RMS |dr| = %.3f km, max |dr| = %.3f km at t=%.0f min, "
           "final = %.3f km\n", std::sqrt(sum2 / n), max_tot, max_min, final_tot);
    return 0;
}
