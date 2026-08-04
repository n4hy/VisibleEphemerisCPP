// test_od_end_to_end.cpp -- Audit fix: end-to-end OD convergence on a
// synthetic Doppler pass. Simulates a truth trajectory via ForceModel +
// integrate_rk4, generates noisy Doppler observations from that truth,
// perturbs the prior state, runs ve::od::run in Mode B (forward + one
// backward smoothing pass), and asserts that the smoothed trajectory
// tracks truth to within tolerance.
//
// This is the missing HIGH-severity coverage flagged in the audit: no
// existing unit test exercised the full driver on the orbital state
// (previous coverage was 2-D linear CV, isolated component checks, or
// benchmarks that are opt-in and unmeasured by ctest).
//
// Newton discipline: the tolerances are stated numerically here and are
// bounded by simulation choices (noise, arc length, geometry). Any change
// to the driver, the SRUKF layer, or the process-noise model that pushes
// these numbers past the stated limits deserves scrutiny.

#include "od/od_smoother.hpp"
#include "od/od_types.hpp"
#include "od/od_dynamics.hpp"
#include "od/doppler_measurement.hpp"
#include "force_model.hpp"
#include "types.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace ve;

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; } \
        else         { std::fprintf(stdout, "ok:   %s\n", msg); }             \
    } while (0)

static TimePoint utc(int y, int mo, int d, int h, int mi, int s) {
    std::tm gm{};
    gm.tm_year = y - 1900; gm.tm_mon = mo - 1; gm.tm_mday = d;
    gm.tm_hour = h; gm.tm_min = mi; gm.tm_sec = s;
    return Clock::from_time_t(timegm(&gm));
}

int main() {
    // ------------------------------------------------------------------
    // Setup: truth orbit, force model, station, oscillator, epoch.
    // Two-body gravity only for exact reproducibility across builds.
    // ------------------------------------------------------------------
    ForceParams fp;
    fp.grav_degree = 0; fp.grav_order = 0;
    fp.use_sun = fp.use_moon = fp.use_drag = fp.use_srp = false;
    ForceModel fm(fp);

    // Canonical 420 km circular ECI orbit along ECI x with velocity along y.
    Vector3 r0{6778.0, 0.0, 0.0};
    Vector3 v0{0.0, std::sqrt(EARTH_MU / 6778.0), 0.0};

    const TimePoint t0 = utc(2024, 6, 15, 12, 0, 0);
    const double jd0 = toJulianDate(t0);

    // Truth trajectory at dt=1s over a 300-second arc.
    const double dt_truth = 1.0;
    const int    N_truth  = 300;
    std::vector<TimePoint> t_truth; t_truth.reserve(N_truth);
    std::vector<Vector3>   r_truth, v_truth;
    r_truth.reserve(N_truth); v_truth.reserve(N_truth);
    {
        Vector3 r = r0, v = v0;
        for (int k = 0; k < N_truth; ++k) {
            auto [r1, v1] = od::integrate_rk4(fm, jd0, k * dt_truth,
                                              r, v, (k + 1) * dt_truth);
            r = r1; v = v1;
            t_truth.push_back(t0 + std::chrono::seconds(k + 1));
            r_truth.push_back(r);
            v_truth.push_back(v);
        }
    }

    // Ground station and downlink.
    Geodetic station{35.0, -106.0, 2.0};
    const double truth_bc = 25.0;    // Hz
    const double truth_bd = 0.01;    // Hz/s
    const double f_T      = 1.62e9;  // Hz (Iridium-ish)
    const double sigma_R  = 5.0;     // Hz noise

    // Noisy Doppler observations at dt=2s cadence.
    std::mt19937 rng(20260804u);
    std::normal_distribution<double> nR(0.0, sigma_R);
    std::vector<od::DopplerObservation> obs;
    for (int k = 0; k < N_truth; k += 2) {
        od::StateVecD xk = od::make_state(r_truth[k], v_truth[k],
                                          truth_bc, truth_bd);
        double f_pred = od::predict_doppler_hz(xk, t_truth[k], f_T, station, t0);
        obs.push_back({t_truth[k], f_pred + nR(rng)});
    }
    CHECK(obs.size() >= 100, "at least 100 Doppler observations generated");

    // ------------------------------------------------------------------
    // Prior: perturb the truth by 100 m alongtrack + 1 m/s alongtrack
    // (well within the RSW prior sigmas below), plus a modest oscillator
    // bias offset. This keeps the initial Doppler-innovation magnitude
    // inside the NLF-default innovation gate (chi^2 = 25 for NY=1) so
    // the SR-UKF sigma-point linearisation is trustworthy from step 1.
    // ------------------------------------------------------------------
    Vector3 alongtrack_hat = v0 * (1.0 / v0.magnitude());
    Vector3 r_prior = r0 + alongtrack_hat * 0.1;       // +100 m alongtrack
    Vector3 v_prior = v0 + alongtrack_hat * 0.001;     // +1 m/s alongtrack
    od::StateVecD x0_prior = od::make_state(r_prior, v_prior, 10.0, 0.0);

    od::FilterConfig cfg;
    cfg.f_transmit_hz = f_T;
    cfg.station       = station;
    cfg.sigma_R_hz    = sigma_R;
    cfg.sigma_process_accel = 1.0e-7;   // km/s^2 sqrt(s)
    cfg.sigma_process_bc    = 0.05;
    cfg.sigma_process_bd    = 0.005;
    cfg.force_params = fp;
    // Prior covariance: RSW-aligned, snug around the actual perturbation
    // magnitudes so the sigma-point spread stays inside the local-linear
    // regime of the Doppler measurement. If these sigmas are much larger
    // than the true perturbation the unscented transform samples highly
    // nonlinear regions and NIS ends up biased for the first few epochs.
    cfg.prior_rsw.sigma_r_radial = 0.1;      // 100 m
    cfg.prior_rsw.sigma_r_along  = 0.2;      // 200 m
    cfg.prior_rsw.sigma_r_cross  = 0.1;      // 100 m
    cfg.prior_rsw.sigma_v_radial = 0.001;    // 1 m/s
    cfg.prior_rsw.sigma_v_along  = 0.002;    // 2 m/s
    cfg.prior_rsw.sigma_v_cross  = 0.001;    // 1 m/s
    cfg.prior_rsw.sigma_bc = 50.0;
    cfg.prior_rsw.sigma_bd = 0.1;
    // Leave innovation gate + reject flag at NLF defaults (the OD guard
    // enforces this).

    od::IterationConfig ic;
    ic.max_iterations = 5;
    ic.tol_state_maha = 1e-6;
    ic.tol_loglik     = 1e-4;

    od::PassInput in;
    in.x0_at_t_ref  = x0_prior;
    in.t_ref_utc    = t0;
    in.jd_at_t_ref  = jd0;
    in.observations = obs;

    // ------------------------------------------------------------------
    // Run Mode B: forward + one backward smoothing pass. With a small
    // (100 m / 1 m/s) prior perturbation, the unscented linearisation is
    // accurate enough that a single smoothing pass suffices. Mode D would
    // give tighter numbers but the audit-value here is a compact
    // "end-to-end still works" check.
    // ------------------------------------------------------------------
    od::PassResult result = od::run(od::Mode::B_ForwardSmooth, in, cfg, ic, fm);

    CHECK(result.converged, "od::run reports converged");
    CHECK(!result.x_smoothed.empty(), "smoothed trajectory produced");
    CHECK(result.x_filtered.size() == result.x_smoothed.size(),
          "filtered and smoothed histories have same length");

    // Match smoothed states to truth epochs.
    // Note: x_smoothed[0] is the prior; x_smoothed[k+1] corresponds to obs[k],
    // which was sampled at truth index 2k (dt=2s obs cadence vs dt=1s truth).
    double sum_pos2 = 0.0, sum_vel2 = 0.0;
    int nn = 0;
    for (std::size_t j = 0; j < obs.size(); ++j) {
        const od::StateVecD& xk = result.x_smoothed[j + 1];
        Vector3 r_est = od::pos_from_state(xk);
        Vector3 v_est = od::vel_from_state(xk);
        int truth_idx = static_cast<int>(2 * j);
        Vector3 dr = r_est - r_truth[truth_idx];
        Vector3 dv = v_est - v_truth[truth_idx];
        sum_pos2 += dr.dot(dr);
        sum_vel2 += dv.dot(dv);
        ++nn;
    }
    const double pos_rms_km = std::sqrt(sum_pos2 / std::max(1, nn));
    const double vel_rms_km_s = std::sqrt(sum_vel2 / std::max(1, nn));
    std::fprintf(stdout, "     smoothed position RMS = %.4f km,  velocity RMS = %.6f km/s\n",
                 pos_rms_km, vel_rms_km_s);

    CHECK(pos_rms_km < 0.5,
          "smoothed position RMS < 500 m over the pass");
    CHECK(vel_rms_km_s < 0.005,
          "smoothed velocity RMS < 5 m/s over the pass");

    // Oscillator recovery at the last epoch.
    const od::StateVecD& x_last = result.x_smoothed.back();
    const double t_last_sec = od::seconds_between(t0, obs.back().t_utc);
    const double bc_last = x_last(od::idx::BC);
    const double bd_last = x_last(od::idx::BD);
    const double bc_truth_at_end = truth_bc + truth_bd * t_last_sec;
    const double bc_err = bc_last - bc_truth_at_end;
    const double bd_err = bd_last - truth_bd;
    std::fprintf(stdout, "     oscillator err: bc=%.3f Hz, bd=%.5f Hz/s\n", bc_err, bd_err);
    CHECK(std::fabs(bc_err) < 25.0, "final oscillator bias error < 25 Hz (5*sigma_R)");
    CHECK(std::fabs(bd_err) < 0.05, "final oscillator drift error < 0.05 Hz/s");

    // NIS consistency: after the transient (skip first 20 epochs), mean NIS
    // should be O(1) for well-tuned NY=1 filter. Loose acceptance band.
    double sum_nis = 0.0;
    int n_nis = 0;
    for (std::size_t k = 20; k < result.nis.size(); ++k) {
        sum_nis += result.nis[k];
        ++n_nis;
    }
    const double mean_nis = (n_nis > 0) ? sum_nis / n_nis : 0.0;
    std::fprintf(stdout, "     mean NIS (post-transient) = %.3f (target ~1.0 for NY=1)\n",
                 mean_nis);
    CHECK(mean_nis > 0.1 && mean_nis < 10.0,
          "post-transient mean NIS in [0.1, 10] (NY=1, chi^2(1) has E=1)");

    // ------------------------------------------------------------------
    // Sanity: also run Mode A (filter only) and confirm the smoother is
    // not WORSE than the filter at the terminal epoch.
    // ------------------------------------------------------------------
    od::PassResult filter_only = od::run(od::Mode::A_FilterOnly, in, cfg, ic, fm);
    Vector3 r_filt_end = od::pos_from_state(filter_only.x_filtered.back());
    Vector3 r_smooth_end = od::pos_from_state(result.x_smoothed.back());
    int truth_end_idx = static_cast<int>(2 * (obs.size() - 1));
    double err_filt   = (r_filt_end   - r_truth[truth_end_idx]).magnitude();
    double err_smooth = (r_smooth_end - r_truth[truth_end_idx]).magnitude();
    // Smoothed error at the terminal epoch should be no larger than the
    // filtered error by more than a small factor (the RTS pass adds future
    // information; it should not systematically hurt any single epoch).
    CHECK(err_smooth <= 3.0 * err_filt + 0.05,
          "smoothed terminal-epoch error not much worse than filtered");

    std::fprintf(stdout, failures ? "\nTESTS FAILED\n" : "\nALL TESTS PASSED\n");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
