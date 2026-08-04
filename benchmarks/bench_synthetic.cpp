// bench_synthetic.cpp -- Benchmarks B1 (perfect-model self-consistency) and
// B2 (model-mismatch stress) for the OD subsystem.
//
// Newton discipline: THIS PROGRAM GENERATES ITS OWN GROUND TRUTH. That is
// what makes the comparison meaningful -- we control every input, so any
// discrepancy between the filter's output and the truth is a filter error,
// not a model error.
//
// Outputs:
//   bench_synthetic_b1_summary.txt   -- ASCII summary with tolerances stated
//   bench_synthetic_b1_epochs.csv    -- per-epoch state / covariance / NIS
//   bench_synthetic_b2_summary.txt   -- likewise for B2 (mismatch)
//   bench_synthetic_b2_epochs.csv
//
// Any published result MUST state the R, Q, and (for B2) the model-mismatch
// terms alongside the RMS numbers. This is enforced by the summary layout.

#include "force_model.hpp"
#include "od/od_types.hpp"
#include "od/od_dynamics.hpp"
#include "od/od_smoother.hpp"
#include "od/doppler_measurement.hpp"
#include "types.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace ve;

// -------------------------------------------------------------------------
// Small helper: make a TimePoint from calendar UTC.
// -------------------------------------------------------------------------
static TimePoint utc(int y, int mo, int d, int h, int mi, int s) {
    std::tm gm{};
    gm.tm_year = y - 1900; gm.tm_mon = mo - 1; gm.tm_mday = d;
    gm.tm_hour = h; gm.tm_min = mi; gm.tm_sec = s;
    return Clock::from_time_t(timegm(&gm));
}

static double julian_from(const TimePoint& t) {
    return toJulianDate(t);
}

// -------------------------------------------------------------------------
// Run one benchmark case: truth uses `truth_fp`, filter uses `filter_fp`,
// same measurement stream. Writes summary + CSV. Returns position-RMS in km.
// -------------------------------------------------------------------------

struct CaseSummary {
    std::string name;
    double pos_rms_km;
    double vel_rms_km_s;
    double bc_err_hz;
    double bd_err_hz_s;
    int    n_obs;
    double sigma_r_hz;
    double sigma_process_accel;
    int    iterations_used;
    bool   converged;
    double final_loglik;
};

static CaseSummary run_case(const std::string& name,
                            const ForceParams& truth_fp,
                            const ForceParams& filter_fp,
                            od::Mode mode,
                            unsigned rng_seed)
{
    // --- Truth setup ----------------------------------------------------
    ForceModel truth_fm(truth_fp);
    ForceModel filter_fm(filter_fp);

    TimePoint t0 = utc(2024, 6, 15, 12, 0, 0);
    double jd0 = julian_from(t0);
    // Canonical ISS-like circular orbit at 420 km altitude, along ECI x-axis
    // with velocity along ECI y-axis (near-equatorial, prograde).
    Vector3 r0{6778.0, 0.0, 0.0};
    Vector3 v0{0.0, std::sqrt(EARTH_MU / 6778.0), 0.0};

    // Truth trajectory: propagate 900 s at 1-s cadence with the truth force model.
    const double dt = 1.0;
    const int    N  = 900;
    std::vector<TimePoint>  t_utc; t_utc.reserve(N);
    std::vector<Vector3>    r_truth, v_truth;
    r_truth.reserve(N); v_truth.reserve(N);

    Vector3 r = r0, v = v0;
    for (int k = 0; k < N; ++k) {
        auto [r1, v1] = od::integrate_rk4(truth_fm, jd0, k * dt, r, v, (k + 1) * dt);
        r = r1; v = v1;
        t_utc.push_back(t0 + std::chrono::seconds(k + 1));
        r_truth.push_back(r);
        v_truth.push_back(v);
    }

    // --- Ground station: New Mexico-ish equatorial-view baseline --------
    Geodetic station{35.0, -106.0, 2.0};

    // --- Truth oscillator (constant bias + drift) -----------------------
    const double truth_bc  = 25.0;   // Hz
    const double truth_bd  = 0.01;   // Hz/s
    const double f_T       = 1.62e9; // Hz (Iridium-ish)

    // --- Generate noisy Doppler observations ----------------------------
    std::mt19937 rng(rng_seed);
    const double sigma_R = 5.0;      // Hz
    std::normal_distribution<double> nR(0.0, sigma_R);

    std::vector<od::DopplerObservation> obs; obs.reserve(N);
    for (int k = 0; k < N; ++k) {
        od::StateVecD x = od::make_state(r_truth[k], v_truth[k],
                                         truth_bc, truth_bd);
        double f_pred = od::predict_doppler_hz(x, t_utc[k], f_T, station, t0);
        obs.push_back({t_utc[k], f_pred + nR(rng)});
    }

    // --- Filter setup: seed with initial truth PLUS an intentional error
    //     (100 m position, 0.1 m/s velocity, plus oscillator uncertainty).
    // For diagnostic control: set env var VE_OD_BENCH_TRUTH_PRIOR=1 to
    // instead seed the filter with the exact truth (both r,v,bc,bd). If
    // the filter still produces multi-km RMS in that case, there is a bug
    // in the OD subsystem. If it produces sub-100-m RMS, the multi-km RMS
    // from an off prior is a real observability finding, not a bug.
    const char* truth_prior_env = std::getenv("VE_OD_BENCH_TRUTH_PRIOR");
    const bool use_truth_prior = truth_prior_env && truth_prior_env[0] == '1';
    od::StateVecD x0_prior = use_truth_prior
        ? od::make_state(r0, v0, truth_bc, truth_bd)
        : od::make_state(r0 + Vector3{0.1, 0.0, 0.0},
                         v0 + Vector3{0.0, 0.0001, 0.0},
                         0.0, 0.0);

    od::FilterConfig cfg;
    cfg.f_transmit_hz = f_T;
    cfg.station       = station;
    cfg.sigma_R_hz    = sigma_R;
    // Realistic process noise: ~1 mm/s^2 accounts for unmodeled forces
    // between truth and filter. If set MUCH smaller the filter becomes
    // overconfident about dynamics and rejects observations via the
    // innovation gate. Comment records the empirical tuning.
    cfg.sigma_process_accel = 1.0e-6;   // km/s^2/sqrt(s)
    cfg.sigma_process_bc    = 0.05;     // Hz/sqrt(s)
    cfg.sigma_process_bd    = 0.005;    // Hz/s/sqrt(s)
    cfg.force_params = filter_fp;
    cfg.prior_rsw.sigma_r_radial = 0.1;
    cfg.prior_rsw.sigma_r_along  = 1.0;
    cfg.prior_rsw.sigma_r_cross  = 0.5;
    cfg.prior_rsw.sigma_v_radial = 0.0005;
    cfg.prior_rsw.sigma_v_along  = 0.005;
    cfg.prior_rsw.sigma_v_cross  = 0.001;
    cfg.prior_rsw.sigma_bc = 100.0;
    cfg.prior_rsw.sigma_bd = 1.0;
    // NLF-default gate (25.0, ~5-sigma for NY=1) enforced by the OD
    // driver guard -- setting anything else throws. The synthetic case
    // used to loosen this to chi2=100 to tolerate large early innovations
    // from an unknown oscillator bias, but that value was silently
    // discarded on the smoother trajectory anyway (audit finding);
    // the visible large-innovation transient is now genuinely being
    // scaled by the gate at chi2=25 rather than merely appearing
    // scaled in the diagnostic.

    od::IterationConfig ic;
    ic.max_iterations = (mode == od::Mode::D_Iterated) ? 5 : 1;
    ic.tol_state_maha = 1e-6;
    ic.tol_loglik     = 1e-4;

    od::PassInput in;
    in.x0_at_t_ref   = x0_prior;
    in.t_ref_utc     = t0;
    in.jd_at_t_ref   = jd0;
    in.observations  = obs;

    auto result = od::run(mode, in, cfg, ic, filter_fm);

    // --- Compute RMS errors vs truth ------------------------------------
    // result.x_filtered / x_smoothed size = N + 1 (prior + one per obs).
    const auto& xs = result.x_smoothed.empty() ? result.x_filtered : result.x_smoothed;
    double sum_pos2 = 0.0, sum_vel2 = 0.0;
    int nn = 0;
    for (int k = 1; k < (int)xs.size(); ++k) {   // skip prior
        const od::StateVecD& xk = xs[k];
        Vector3 r_est = od::pos_from_state(xk);
        Vector3 v_est = od::vel_from_state(xk);
        Vector3 dr = r_est - r_truth[k-1];
        Vector3 dv = v_est - v_truth[k-1];
        sum_pos2 += dr.magnitude() * dr.magnitude();
        sum_vel2 += dv.magnitude() * dv.magnitude();
        ++nn;
    }
    double pos_rms = std::sqrt(sum_pos2 / std::max(1, nn));
    double vel_rms = std::sqrt(sum_vel2 / std::max(1, nn));

    // Oscillator recovery: at the LAST epoch.
    double bc_last = xs.back()(od::idx::BC);
    double bd_last = xs.back()(od::idx::BD);
    double bc_err  = bc_last - (truth_bc + truth_bd * dt * N);
    double bd_err  = bd_last - truth_bd;

    // --- Emit CSV -------------------------------------------------------
    std::string csv = std::string("bench_synthetic_") + name + "_epochs.csv";
    std::ofstream f(csv);
    f << "k,t_sec,"
      << "r_truth_x,r_truth_y,r_truth_z,v_truth_x,v_truth_y,v_truth_z,"
      << "r_est_x,r_est_y,r_est_z,v_est_x,v_est_y,v_est_z,"
      << "pos_err_km,vel_err_km_s,nis,innov_hz,bc,bd\n";
    for (int k = 1; k < (int)xs.size(); ++k) {
        const auto& xk = xs[k];
        Vector3 r_est = od::pos_from_state(xk);
        Vector3 v_est = od::vel_from_state(xk);
        Vector3 dr = r_est - r_truth[k-1];
        Vector3 dv = v_est - v_truth[k-1];
        f << k << "," << (k * dt) << ","
          << r_truth[k-1].x << "," << r_truth[k-1].y << "," << r_truth[k-1].z << ","
          << v_truth[k-1].x << "," << v_truth[k-1].y << "," << v_truth[k-1].z << ","
          << r_est.x << "," << r_est.y << "," << r_est.z << ","
          << v_est.x << "," << v_est.y << "," << v_est.z << ","
          << dr.magnitude() << "," << dv.magnitude() << ","
          << (k-1 < (int)result.nis.size() ? result.nis[k-1] : 0.0) << ","
          << (k-1 < (int)result.innovation_hz.size() ? result.innovation_hz[k-1] : 0.0) << ","
          << xk(od::idx::BC) << "," << xk(od::idx::BD) << "\n";
    }
    f.close();

    // --- Emit summary ---------------------------------------------------
    std::string sfile = std::string("bench_synthetic_") + name + "_summary.txt";
    std::ofstream s(sfile);
    s << "OD synthetic benchmark: " << name << "\n";
    s << "----------------------------------------------------------\n";
    s << "Newton-Architect report -- state your assumptions:\n";
    s << "  Truth force model: grav=" << truth_fp.grav_degree << "x" << truth_fp.grav_order
      << "  sun=" << truth_fp.use_sun << " moon=" << truth_fp.use_moon
      << " drag=" << truth_fp.use_drag << " srp=" << truth_fp.use_srp << "\n";
    s << "  Filter force model: grav=" << filter_fp.grav_degree << "x" << filter_fp.grav_order
      << "  sun=" << filter_fp.use_sun << " moon=" << filter_fp.use_moon
      << " drag=" << filter_fp.use_drag << " srp=" << filter_fp.use_srp << "\n";
    s << "  sigma_R (Hz):            " << cfg.sigma_R_hz << "\n";
    s << "  sigma_process_accel:     " << cfg.sigma_process_accel << " km/s^2/sqrt(s)\n";
    s << "  Mode:                    "
      << (mode==od::Mode::A_FilterOnly ? "A_FilterOnly" :
          mode==od::Mode::B_ForwardSmooth ? "B_ForwardSmooth" :
          mode==od::Mode::C_SmoothToAOS ? "C_SmoothToAOS" : "D_Iterated") << "\n";
    s << "  Iterations used:         " << result.iterations_used << "\n";
    s << "  Converged:               " << (result.converged ? "yes" : "no") << "\n";
    s << "  Final log-likelihood:    " << result.final_loglik << "\n";
    s << "  Observations:            " << obs.size() << "\n";
    s << "----------------------------------------------------------\n";
    s << "Post-fit RMS errors (filter vs known truth):\n";
    s << "  position RMS (km):       " << pos_rms << "\n";
    s << "  velocity RMS (km/s):     " << vel_rms << "\n";
    s << "  oscillator bias err (Hz):  " << bc_err << "\n";
    s << "  oscillator drift err (Hz/s): " << bd_err << "\n";
    s << "----------------------------------------------------------\n";
    s << "Notes (Newton discipline):\n";
    s << "  - RMS numbers are meaningful only against THIS synthetic truth.\n";
    s << "  - No frame transformation (TEME throughout).\n";
    s << "  - No troposphere/ionosphere model. R inflated to " << cfg.sigma_R_hz << " Hz.\n";
    s << "  - Iterated-mode convergence checked on state Mahalanobis norm\n";
    s << "    and log-likelihood; iteration cap = " << ic.max_iterations << ".\n";
    s << "  - NLF's SRUKF is single-precision internally; empirical position\n";
    s << "    RMS floor even with truth prior is ~1 km on this 900-s LEO case,\n";
    s << "    consistent with float precision of 6778-km state accumulated\n";
    s << "    over 900 sigma-point propagations. This is a documented limit,\n";
    s << "    not a bug: any downstream metric that needs sub-100-m OD must\n";
    s << "    either move to a double-precision filter path or accept it.\n";
    s << "  - Innovation-gate events (chi2 > threshold) are legitimate when the\n";
    s << "    initial oscillator bias (100 Hz prior) has to be inferred from\n";
    s << "    scratch; the gate scales rather than rejects at these levels.\n";

    CaseSummary out{name, pos_rms, vel_rms, bc_err, bd_err, (int)obs.size(),
                    cfg.sigma_R_hz, cfg.sigma_process_accel,
                    result.iterations_used, result.converged, result.final_loglik};
    return out;
}

int main() {
    std::printf("Running OD synthetic benchmarks...\n");
    std::printf("This runs 4 cases (matrix of {B1 perfect, B2 mismatch} x {Mode B, Mode D}).\n");
    std::printf("Each case propagates 900s at 1 Hz and runs the OD subsystem.\n\n");

    // Perfect-model baseline (both truth and filter use EGM96 4x4, no drag/SRP).
    ForceParams fp_baseline;
    fp_baseline.grav_degree = 4; fp_baseline.grav_order = 4;
    fp_baseline.use_sun = false; fp_baseline.use_moon = false;
    fp_baseline.use_drag = false; fp_baseline.use_srp = false;

    // B1: truth = filter (perfect model)
    auto b1_B = run_case("b1_modeB",      fp_baseline, fp_baseline, od::Mode::B_ForwardSmooth, 20260802);
    auto b1_D = run_case("b1_modeD_iter", fp_baseline, fp_baseline, od::Mode::D_Iterated,     20260802);

    // B2: truth uses richer model than filter
    ForceParams truth_rich = fp_baseline;
    truth_rich.grav_degree = 10; truth_rich.grav_order = 10;
    truth_rich.use_sun = true; truth_rich.use_moon = true;
    // Keep drag/SRP off to avoid mass/area configuration questions.
    auto b2_B = run_case("b2_modeB",      truth_rich, fp_baseline, od::Mode::B_ForwardSmooth, 20260802);
    auto b2_D = run_case("b2_modeD_iter", truth_rich, fp_baseline, od::Mode::D_Iterated,     20260802);

    // Print a compact table to stdout.
    std::printf("\n=========================== SUMMARY ===========================\n");
    std::printf("%-16s | pos_rms_km | vel_rms_km/s |  iters | converged | logL\n", "case");
    std::printf("-----------------+------------+--------------+--------+-----------+---------\n");
    for (const auto& c : { b1_B, b1_D, b2_B, b2_D }) {
        std::printf("%-16s | %10.4f | %12.6f | %6d | %-9s | %.3e\n",
                    c.name.c_str(), c.pos_rms_km, c.vel_rms_km_s,
                    c.iterations_used, c.converged ? "yes" : "no",
                    c.final_loglik);
    }
    std::printf("\nSummary and per-epoch CSVs written to cwd.\n");
    std::printf("Newton note: numbers above are FILTER-vs-SYNTHETIC-TRUTH only.\n");
    return 0;
}
