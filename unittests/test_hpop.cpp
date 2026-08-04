// Unit tests for the High-Precision Orbit Propagator (HPOP).
//
// Verifies the correctness properties that matter:
//   1. The TLE -> state-vector seed: HPOP at the epoch equals SGP4 at the epoch.
//   2. Integrator fidelity: two-body energy (semi-major axis) is conserved.
//   3. Full geopotential keeps the osculating semi-major axis bounded.
//   4. B* -> ballistic-coefficient conversion (Cd*A/m = 2*B*/0.15696615).
//   5. Sun/Moon analytic ephemerides return physically sane distances.
#include <iostream>
#include <cmath>
#include <cstdlib>
#include "../include/numerical_propagator.hpp"
#include "../include/force_model.hpp"
#include "../include/ephemeris.hpp"
#include "../include/egm96_coeffs.hpp"
#include <Tle.h>
#include <SGP4.h>
#include <Eci.h>

using namespace ve;

static int failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::cerr << "FAIL: " << (msg) << "\n"; ++failures;} \
                              else { std::cout << "ok: " << (msg) << "\n"; } } while(0)

static double mag(const Vector3& v){ return std::sqrt(v.dot(v)); }
static double sma(const Vector3& r, const Vector3& v){
    double mu = egm96::GM_KM3_S2, R = mag(r), V2 = v.dot(v);
    return -mu / (2.0 * (V2/2.0 - mu/R));
}

int main(){
    const std::string name = "ISS (ZARYA)";
    const std::string l1 = "1 25544U 98067A   24079.07757601  .00016717  00000-0  30074-3 0  9993";
    const std::string l2 = "2 25544  51.6393 110.7232 0005383  62.8273  29.0235 15.49648268446247";
    libsgp4::Tle tle(name, l1, l2);
    libsgp4::SGP4 sgp4(tle);

    ForceParams fp; fp.bstar = tle.BStar();
    NumericalPropagator prop(tle, fp);
    CHECK(prop.valid(), "propagator initialized");

    // 1. Epoch seed equals SGP4-at-epoch.
    auto sv0 = prop.stateAtSeconds(0.0);
    libsgp4::Eci e0 = sgp4.FindPosition(tle.Epoch());
    Vector3 sr0{e0.Position().x, e0.Position().y, e0.Position().z};
    Vector3 svel0{e0.Velocity().x, e0.Velocity().y, e0.Velocity().z};
    CHECK(mag(sv0.first - sr0) < 1e-6, "HPOP(t=0) position == SGP4(epoch)");
    CHECK(mag(sv0.second - svel0) < 1e-9, "HPOP(t=0) velocity == SGP4(epoch)");

    // 2. Two-body only: semi-major axis conserved to < 1 m over a day.
    ForceParams f2; f2.use_sun=f2.use_moon=f2.use_drag=f2.use_srp=false;
    f2.grav_degree=0; f2.grav_order=0;
    NumericalPropagator p2(tle, f2);
    auto a0 = p2.stateAtSeconds(0.0);
    auto a1 = p2.stateAtSeconds(86400.0);
    double dsma = std::fabs(sma(a1.first,a1.second) - sma(a0.first,a0.second));
    CHECK(dsma < 1e-3, "two-body semi-major axis conserved over 1 day (<1 m)");

    // 3. Full geopotential: osculating sma stays bounded over a day.
    ForceParams fg; fg.use_sun=fg.use_moon=fg.use_drag=fg.use_srp=false;
    NumericalPropagator pg(tle, fg);
    auto g0 = pg.stateAtSeconds(0.0);
    auto g1 = pg.stateAtSeconds(86400.0);
    CHECK(std::fabs(sma(g1.first,g1.second) - sma(g0.first,g0.second)) < 5.0,
          "geopotential osculating sma bounded over 1 day (<5 km)");

    // 4. B* -> Cd*A/m conversion.
    double expect = 2.0 * tle.BStar() / 0.15696615;
    CHECK(std::fabs(prop.forceModel().cdAoverM() - expect) < 1e-9, "Cd*A/m derived from B*");
    CHECK(prop.forceModel().dragEnabled(), "drag enabled for ISS");

    // 5. Sun/Moon distances.
    double jd = tle.Epoch().ToJulian();
    double rs = mag(sunPositionECI(jd)), rm = mag(moonPositionECI(jd));
    CHECK(rs > 1.45e8 && rs < 1.53e8, "Sun distance physically sane");
    CHECK(rm > 3.5e5 && rm < 4.1e5, "Moon distance physically sane");

    // 6. Atmosphere density monotonic-ish decrease with altitude.
    CHECK(ForceModel::atmosphereDensity(200) > ForceModel::atmosphereDensity(400),
          "atmospheric density decreases with altitude");

    // 7. Precession rotation properties (audit fix: J2000 -> MOD/TEME).
    {
        // At J2000 the precession angles are zero, so the rotation is identity.
        Vector3 v{1.0, 2.0, 3.0};
        Vector3 v_id = precessJ2000ToMOD(2451545.0, v);
        CHECK(mag(v_id - v) < 1e-15,
              "precessJ2000ToMOD is identity at J2000 epoch");

        // Rotation must be length-preserving (orthogonal).
        Vector3 e1{1.0, 0.0, 0.0}, e2{0.0, 1.0, 0.0}, e3{0.0, 0.0, 1.0};
        Vector3 r1 = precessJ2000ToMOD(jd, e1);
        Vector3 r2 = precessJ2000ToMOD(jd, e2);
        Vector3 r3 = precessJ2000ToMOD(jd, e3);
        const double lenTol = 1e-13;
        CHECK(std::fabs(mag(r1) - 1.0) < lenTol
              && std::fabs(mag(r2) - 1.0) < lenTol
              && std::fabs(mag(r3) - 1.0) < lenTol,
              "precession rotates unit vectors to unit vectors");
        // Orthogonality of the rotated basis (matrix rows are orthonormal too).
        CHECK(std::fabs(r1.dot(r2)) < 1e-13
              && std::fabs(r1.dot(r3)) < 1e-13
              && std::fabs(r2.dot(r3)) < 1e-13,
              "precession preserves orthogonality of basis vectors");

        // TEME/MOD Sun/Moon distances match J2000 magnitudes (rotation-invariant).
        double rs_teme = mag(sunPositionTEME(jd));
        double rm_teme = mag(moonPositionTEME(jd));
        CHECK(std::fabs(rs_teme - rs) < 1e-6,
              "sunPositionTEME preserves geocentric distance from sunPositionECI");
        CHECK(std::fabs(rm_teme - rm) < 1e-6,
              "moonPositionTEME preserves geocentric distance from moonPositionECI");

        // For a modern epoch (~26 years post-J2000) the angular rotation of the
        // Sun should be of order 0.4 deg (dominant precession-in-longitude).
        // Confirm it is nonzero and small.
        Vector3 s_j2000 = sunPositionECI(jd);
        Vector3 s_teme  = sunPositionTEME(jd);
        double cos_ang = s_j2000.dot(s_teme) / (mag(s_j2000) * mag(s_teme));
        // clamp for floating point
        if (cos_ang > 1.0) cos_ang = 1.0;
        double ang_deg = std::acos(cos_ang) * 180.0 / PI;
        CHECK(ang_deg > 1e-4 && ang_deg < 1.0,
              "Sun J2000 vs TEME angular separation is between 1e-4 and 1 degree");
    }

    std::cout << (failures ? "\nTESTS FAILED\n" : "\nALL TESTS PASSED\n");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
