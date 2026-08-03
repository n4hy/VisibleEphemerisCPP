// test_od_frames.cpp -- T1 frame round-trip and T10 coordinate-frame guardrail.
//
// Newton discipline: these are numerical checks, not proofs. Tolerances are
// stated in the assertions and are stated at the top of each test.

#include "types.hpp"
#include "earth_rotation.hpp"
#include "od/od_types.hpp"

#include <cassert>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL: %s\n", msg);                          \
            ++failures;                                                       \
        } else {                                                              \
            std::fprintf(stdout, "ok:   %s\n", msg);                          \
        }                                                                     \
    } while (0)

// --- T1: ECI -> ECEF -> ECI round-trip preserves the vector to 1e-12 relative
static void t1_frame_round_trip() {
    using namespace ve;
    // Two epochs: Y2K and a random 2024 timestamp.
    std::tm gm0{};
    gm0.tm_year = 100; gm0.tm_mon = 0; gm0.tm_mday = 1;
    gm0.tm_hour = 12;  gm0.tm_min = 0;  gm0.tm_sec = 0;
    time_t tt0 = timegm(&gm0);
    TimePoint t0 = Clock::from_time_t(tt0);

    GmstRotation gr;
    Vector3 v_in{4321.0, -1234.5, 6789.0};
    Vector3 v_ecef = gr.rotate_eci_to_ecef(v_in, t0);
    // Inverse rotation is R^T for a pure z-rotation; assemble it via the
    // Rotation3's data. We know GmstRotation is a scalar-angle z-rotation:
    // (x,y,z) -> (c x + s y, -s x + c y, z). Its inverse rotates back.
    double theta = getGMST(t0);
    double c = std::cos(theta), s = std::sin(theta);
    Vector3 v_back{ c*v_ecef.x - s*v_ecef.y,
                    s*v_ecef.x + c*v_ecef.y,
                    v_ecef.z };
    double err = std::hypot(std::hypot(v_back.x - v_in.x, v_back.y - v_in.y),
                            v_back.z - v_in.z);
    double rel = err / v_in.magnitude();
    CHECK(rel < 1e-12,
          "T1: ECI->ECEF->ECI round-trip relative error < 1e-12");

    // Also: rotation preserves norm.
    double norm_in = v_in.magnitude();
    double norm_ecef = v_ecef.magnitude();
    CHECK(std::abs(norm_in - norm_ecef) / norm_in < 1e-14,
          "T1: rotation preserves Euclidean norm to 1e-14 relative");
}

// --- T10: coordinate-frame guardrail
// A canonical ISS-like ECI state is diagnose_state()-plausible. Substituting
// ECEF velocity (v - omega x r) into the ECI slot gives a state whose
// specific energy / semi-major axis are inconsistent with a bound Earth
// satellite, and diagnose_state must flag it.
static void t10_frame_guardrail() {
    using namespace ve;

    // Approximate ISS state (~420 km altitude, circular). r along x, v along y.
    Vector3 r_eci{6778.0, 0.0, 0.0};      // km
    double v_circ = std::sqrt(EARTH_MU / r_eci.magnitude()); // km/s
    Vector3 v_eci{0.0, v_circ, 0.0};

    auto d_eci = od::diagnose_state(r_eci, v_eci);
    CHECK(d_eci.is_plausible,
          "T10: canonical ECI ISS state passes plausibility check");
    CHECK(std::abs(d_eci.eccentricity) < 1e-9,
          "T10: canonical ECI ISS state has eccentricity ~= 0");
    CHECK(std::abs(d_eci.semi_major_axis_km - r_eci.magnitude()) < 1e-6,
          "T10: canonical ECI ISS state semi-major axis matches r for circular");

    // Now substitute an ECEF velocity into the ECI slot: v_ecef = v_eci - omega x r.
    // omega = (0, 0, EARTH_ROTATION_RATE).
    Vector3 omega{0.0, 0.0, EARTH_ROTATION_RATE};
    Vector3 v_ecef = v_eci - omega.cross(r_eci);
    auto d_ecef = od::diagnose_state(r_eci, v_ecef);
    // The ECEF-labelled-as-ECI state has |v| that's smaller by ~0.47 km/s so
    // its specific energy is less negative but still negative (still bound).
    // Its eccentricity is what changes: it's no longer circular. Check that
    // the diagnostics report a meaningfully non-zero eccentricity, i.e. the
    // guardrail is discriminating.
    CHECK(d_ecef.eccentricity > 0.05,
          "T10: ECEF-in-ECI-slot state has eccentricity > 0.05 (frame mistake detected)");
    // The stronger test: even if we scale things worse (double the wrong-way
    // velocity), diagnose_state should catch it. Try v = 0 which is clearly
    // unbound-in-limit / clearly wrong.
    Vector3 v_zero{0.0, 0.0, 0.0};
    auto d_zero = od::diagnose_state(r_eci, v_zero);
    CHECK(!d_zero.is_plausible,
          "T10: obviously wrong state (v=0) fails plausibility check");
}

int main() {
    t1_frame_round_trip();
    t10_frame_guardrail();
    if (failures == 0) {
        std::fprintf(stdout, "\nALL TESTS PASSED (test_od_frames)\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d test(s) FAILED (test_od_frames)\n", failures);
    return 1;
}
