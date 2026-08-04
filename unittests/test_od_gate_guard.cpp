// test_od_gate_guard.cpp -- Verifies the audit-fix guard against the silent
// discard of caller-supplied innovation-gate / reject-outliers settings.
//
// NLF's SRUKFSmoother does not surface its internal SRUKF, so the OD driver
// cannot wire caller-specified gate settings into the smoother trajectory.
// od::run must refuse to run when the caller asks for anything other than
// NLF's built-in defaults, rather than silently applying different values
// to the "monitor" SRUKF than to the trajectory-producing smoother.

#include "od/od_smoother.hpp"
#include "od/od_types.hpp"
#include "force_model.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; } \
        else         { std::fprintf(stdout, "ok:   %s\n", msg); }             \
    } while (0)

static ve::od::PassInput trivial_input() {
    ve::od::PassInput in;
    in.t_ref_utc = ve::TimePoint{};
    in.jd_at_t_ref = 2460501.5;  // arbitrary
    // Plausible LEO ECI-TEME state so diagnose_state() accepts it if reached.
    ve::Vector3 r{6778.0, 0.0, 0.0};
    double v_c = std::sqrt(ve::EARTH_MU / 6778.0);
    ve::Vector3 v{0.0, v_c, 0.0};
    in.x0_at_t_ref = ve::od::make_state(r, v);
    // No observations -- the guard fires before the loop is entered.
    return in;
}

int main() {
    using namespace ve::od;

    ve::ForceParams fp;
    fp.grav_degree = 2; fp.grav_order = 0;
    fp.use_sun = fp.use_moon = fp.use_drag = fp.use_srp = false;
    ve::ForceModel fm(fp);

    IterationConfig it_cfg;
    PassInput input = trivial_input();

    // 1. Defaults must be accepted.
    {
        FilterConfig cfg;
        cfg.f_transmit_hz = 1.6e9;
        cfg.station = ve::Geodetic{0.0, 0.0, 0.0};
        bool threw = false;
        try { (void)run(Mode::A_FilterOnly, input, cfg, it_cfg, fm); }
        catch (const std::exception&) { threw = true; }
        // With zero observations the run returns trivially without throwing.
        CHECK(!threw, "default cfg.innovation_gate_chi2 (NLF default 25.0) accepted");
    }

    // 2. A non-default gate value must be rejected loudly.
    {
        FilterConfig cfg;
        cfg.f_transmit_hz = 1.6e9;
        cfg.station = ve::Geodetic{0.0, 0.0, 0.0};
        cfg.innovation_gate_chi2 = 9.0;  // caller wants 3-sigma for NY=1
        bool threw = false;
        std::string what;
        try { (void)run(Mode::A_FilterOnly, input, cfg, it_cfg, fm); }
        catch (const std::exception& e) { threw = true; what = e.what(); }
        CHECK(threw, "non-default innovation_gate_chi2 rejected by guard");
        CHECK(what.find("innovation_gate_chi2") != std::string::npos,
              "guard error message names innovation_gate_chi2");
    }

    // 3. reject_outliers=true must be rejected loudly.
    {
        FilterConfig cfg;
        cfg.f_transmit_hz = 1.6e9;
        cfg.station = ve::Geodetic{0.0, 0.0, 0.0};
        cfg.reject_outliers = true;
        bool threw = false;
        std::string what;
        try { (void)run(Mode::A_FilterOnly, input, cfg, it_cfg, fm); }
        catch (const std::exception& e) { threw = true; what = e.what(); }
        CHECK(threw, "reject_outliers=true rejected by guard");
        CHECK(what.find("reject_outliers") != std::string::npos,
              "guard error message names reject_outliers");
    }

    // 4. The NLF-default constant must in fact equal NLF's compiled-in default
    //    (25.0). This is a compile-time consistency check.
    static_assert(FilterConfig::NLF_DEFAULT_INNOVATION_GATE_CHI2 == 25.0,
                  "FilterConfig::NLF_DEFAULT_INNOVATION_GATE_CHI2 must match "
                  "NLF SRUKF's default innovation_gate_chi2_ = 25.0f");

    std::fprintf(stdout, failures ? "\nTESTS FAILED\n" : "\nALL TESTS PASSED\n");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
