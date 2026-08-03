// test_od_propagator.cpp -- T8 sigma-point RK4 round-trip and T9 TLE->TEME
// state matches libsgp4's own SGP4-at-epoch.
//
// Newton discipline: T8 is a self-consistency check (forward-back through
// RK4). T9 shows that ve::od::teme_state_at_epoch is a THIN wrapper around
// libsgp4's SGP4::FindPosition(epoch) -- if libsgp4's own answer is wrong,
// so is ours, and that would be a libsgp4 bug, not an OD-subsystem bug.

#include "od/od_dynamics.hpp"
#include "od/tle_to_state.hpp"
#include "force_model.hpp"

#include <Tle.h>
#include <SGP4.h>

#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; } \
        else         { std::fprintf(stdout, "ok:   %s\n", msg); }             \
    } while (0)

// Real ISS TLE from tle_cache/stations.txt (2026-07-28 snapshot). We use a
// real element set rather than a synthetic one so libsgp4's line-length /
// checksum validation is satisfied. Only used for a frame identity check;
// the exact epoch is not important.
static const char* ISS_NAME = "ISS (ZARYA)";
static const char* ISS_LN1  = "1 25544U 98067A   26209.15252568  .00010831  00000+0  20282-3 0  9992";
static const char* ISS_LN2  = "2 25544  51.6320  97.3682 0007093 345.6120  14.4666 15.49220842578109";

// -------------------------------------------------------------------------
// T8: RK4 round-trip. Propagate forward T_forward seconds, then backward
// T_forward seconds, and compare to the initial state. RK4 is not
// symplectic and not time-reversible in the strict sense, but the numerical
// truncation error at LEO for dt=0.5s over ~1000 s should keep the round-
// trip to well under 100 m.
// -------------------------------------------------------------------------

static void t8_rk4_round_trip() {
    using namespace ve;

    // Set up a simple ForceModel with two-body only (turn off Sun/Moon/drag/SRP
    // for a clean, deterministic reversibility test).
    ForceParams fp;
    fp.grav_degree = 2; fp.grav_order = 0;  // J2 only
    fp.use_sun = false; fp.use_moon = false;
    fp.use_drag = false; fp.use_srp = false;
    ForceModel fm(fp);

    Vector3 r0{6778.0, 0.0, 0.0};
    double v_circ = std::sqrt(EARTH_MU / r0.magnitude());
    Vector3 v0{0.0, v_circ, 0.0};

    // Use JD2000 as a benign reference date.
    const double jd_ref = 2451545.0;
    const double t_forward = 300.0;   // 5 minutes

    auto [r1, v1] = od::integrate_rk4(fm, jd_ref, 0.0, r0, v0,  t_forward);
    auto [r2, v2] = od::integrate_rk4(fm, jd_ref, t_forward, r1, v1, 0.0);

    Vector3 dr = r2 - r0;
    Vector3 dv = v2 - v0;
    double dr_norm = dr.magnitude();
    double dv_norm = dv.magnitude();

    char msg[192];
    std::snprintf(msg, sizeof msg,
        "T8: RK4 round-trip 300s forward+back |dr| = %.6e km, |dv| = %.6e km/s",
        dr_norm, dv_norm);
    std::fprintf(stdout, "info: %s\n", msg);

    CHECK(dr_norm < 1e-3, "T8: round-trip position error < 1 m over 300 s");
    CHECK(dv_norm < 1e-6, "T8: round-trip velocity error < 1 mm/s over 300 s");
}

// -------------------------------------------------------------------------
// T9: TLE -> teme_state_at_epoch matches libsgp4's own SGP4::FindPosition
// at the same epoch. This ONE test is intentionally an identity check --
// the intent is to make it impossible to accidentally introduce a frame
// rotation into the helper without breaking the assertion.
// -------------------------------------------------------------------------

static void t9_tle_frame_documentation() {
    try {
        libsgp4::Tle tle(ISS_NAME, ISS_LN1, ISS_LN2);
        libsgp4::SGP4 sgp4(tle);
        libsgp4::Eci ref = sgp4.FindPosition(tle.Epoch());

        auto ours = ve::od::teme_state_at_epoch(tle);
        double dr = std::sqrt(
            (ours.r_km.x - ref.Position().x) * (ours.r_km.x - ref.Position().x) +
            (ours.r_km.y - ref.Position().y) * (ours.r_km.y - ref.Position().y) +
            (ours.r_km.z - ref.Position().z) * (ours.r_km.z - ref.Position().z));
        double dv = std::sqrt(
            (ours.v_km_s.x - ref.Velocity().x) * (ours.v_km_s.x - ref.Velocity().x) +
            (ours.v_km_s.y - ref.Velocity().y) * (ours.v_km_s.y - ref.Velocity().y) +
            (ours.v_km_s.z - ref.Velocity().z) * (ours.v_km_s.z - ref.Velocity().z));

        CHECK(dr < 1e-9,
              "T9: teme_state_at_epoch position matches libsgp4 identically");
        CHECK(dv < 1e-12,
              "T9: teme_state_at_epoch velocity matches libsgp4 identically");

        // Also check JD conversion round-trip within microsecond tolerance
        // (Julian Date precision at these epochs is ~1e-8 day = ~1 ms).
        double jd_ref = tle.Epoch().ToJulian();
        CHECK(std::abs(ours.jd_epoch - jd_ref) < 1e-9,
              "T9: teme_state_at_epoch JD matches TLE epoch");

    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: T9 threw: %s\n", e.what());
        ++failures;
    }
}

int main() {
    t8_rk4_round_trip();
    t9_tle_frame_documentation();
    if (failures == 0) {
        std::fprintf(stdout, "\nALL TESTS PASSED (test_od_propagator)\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d test(s) FAILED (test_od_propagator)\n", failures);
    return 1;
}
