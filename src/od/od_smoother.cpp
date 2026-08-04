// od_smoother.cpp -- Mode A/B/C/D orchestration for the OD subsystem.
//
// Reads the NLF SRUKFSmoother history to expose filtered and smoothed states
// through a double-precision PassResult that carries covariance, NIS, and
// per-epoch innovation.
//
// Newton note: this driver deliberately does NOT tune Q or R on the fly to
// make residuals look smaller. If a pass's residuals blow the innovation
// gate repeatedly, that is reported (via PassResult.nis and any warnings)
// and it is the user's job to decide whether the model or the data is
// at fault -- not this code's.

#include "od/od_smoother.hpp"
#include "od/orbit_ssm.hpp"

#include "SRUKF.h"
#include "SRUKFSmoother.h"

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <algorithm>

namespace ve::od {

namespace {

// Convert a NLF-side (float, 8) state to our (double, 8).
StateVecD to_double(const OrbitStateSpaceModel::State& xf) {
    StateVecD xd;
    for (int i = 0; i < OD_STATE_DIM; ++i) xd(i) = static_cast<double>(xf(i));
    return xd;
}

StateMatD to_double(const OrbitStateSpaceModel::StateMat& Pf) {
    StateMatD Pd;
    for (int r = 0; r < OD_STATE_DIM; ++r)
        for (int c = 0; c < OD_STATE_DIM; ++c)
            Pd(r, c) = static_cast<double>(Pf(r, c));
    return Pd;
}

// Convert a float 8-vector to an Eigen (double) 8-vector.
OrbitStateSpaceModel::State to_float(const StateVecD& xd) {
    OrbitStateSpaceModel::State xf;
    for (int i = 0; i < OD_STATE_DIM; ++i) xf(i) = static_cast<float>(xd(i));
    return xf;
}

OrbitStateSpaceModel::StateMat to_float(const StateMatD& Pd) {
    OrbitStateSpaceModel::StateMat Pf;
    for (int r = 0; r < OD_STATE_DIM; ++r)
        for (int c = 0; c < OD_STATE_DIM; ++c)
            Pf(r, c) = static_cast<float>(Pd(r, c));
    return Pf;
}

// Mahalanobis-norm change between two smoothed trajectories.
// Uses the covariance of the FIRST (previous) trajectory to weight the
// difference. Skips epochs where the covariance is not PSD (falls back to
// unweighted Euclidean).
double smoothed_state_change_maha(
    const std::vector<StateVecD>& xs_prev,
    const std::vector<StateMatD>& Ps_prev,
    const std::vector<StateVecD>& xs_curr)
{
    if (xs_prev.size() != xs_curr.size() || xs_prev.empty()) return 0.0;
    double worst = 0.0;
    for (std::size_t k = 0; k < xs_prev.size(); ++k) {
        const StateVecD dx = xs_curr[k] - xs_prev[k];
        // Solve P dz = dx (Mahalanobis d^2 = dx^T P^-1 dx). Use LLT.
        Eigen::LLT<StateMatD> llt(Ps_prev[k]);
        double d2;
        if (llt.info() == Eigen::Success) {
            const StateVecD z = llt.solve(dx);
            d2 = dx.dot(z);
        } else {
            d2 = dx.squaredNorm();
        }
        const double d = std::sqrt(std::max(0.0, d2));
        if (d > worst) worst = d;
    }
    return worst;
}

} // namespace

PassResult run(Mode mode,
               const PassInput& input,
               const FilterConfig& cfg_in,
               const IterationConfig& it_cfg,
               const ve::ForceModel& fm)
{
    // Copy the caller's config so we can pin t_ref onto it.
    FilterConfig cfg = cfg_in;
    cfg.t_ref_utc = input.t_ref_utc;

    if (cfg.f_transmit_hz <= 0.0) {
        throw std::runtime_error(
            "od::run: cfg.f_transmit_hz must be > 0 (physical transmit frequency)");
    }
    // Chronology check: reject earlier-than-t_ref observations rather than
    // silently reordering. See Newton rule against hidden-assumption fixes.
    for (const auto& obs : input.observations) {
        if (obs.t_utc < input.t_ref_utc) {
            throw std::runtime_error(
                "od::run: observation earlier than t_ref rejected "
                "(no silent reordering)");
        }
    }
    // NEWTON audit fix: NLF's SRUKFSmoother holds its SRUKF as a private
    // member with no accessor, so the driver cannot wire caller-specified
    // gate/reject settings into the actual filter that produces the
    // returned trajectory. Refuse to run rather than silently discard.
    // See FilterConfig::NLF_DEFAULT_INNOVATION_GATE_CHI2 for the constraint.
    if (cfg.innovation_gate_chi2 != FilterConfig::NLF_DEFAULT_INNOVATION_GATE_CHI2) {
        throw std::runtime_error(
            "od::run: cfg.innovation_gate_chi2 must equal "
            "FilterConfig::NLF_DEFAULT_INNOVATION_GATE_CHI2 (25.0); "
            "the underlying NLF SRUKFSmoother does not expose gate wiring, "
            "so any other value would be silently ignored on the smoother "
            "trajectory. Patch NLF or leave at the default.");
    }
    if (cfg.reject_outliers) {
        throw std::runtime_error(
            "od::run: cfg.reject_outliers=true not supported: the "
            "underlying NLF SRUKFSmoother does not expose the reject/scale "
            "flag; leaving it at true would apply only to the monitor SRUKF "
            "used for NIS reporting, not the returned trajectory.");
    }
    // Build the OD state-space model and both filter and smoother.
    OrbitStateSpaceModel model(fm, cfg, input.jd_at_t_ref);

    // Initial (float) state and covariance from RSW-based prior.
    const StateVecD x0_d = input.x0_at_t_ref;
    const ve::Vector3 r0v = pos_from_state(x0_d);
    const ve::Vector3 v0v = vel_from_state(x0_d);
    const StateMatD P0_d = build_prior_covariance(r0v, v0v, cfg.prior_rsw);
    // Preserve the ORIGINAL P0 for iterated mode: NLF's SRUKFSmoother::smooth
    // already re-uses the prior it was initialize()d with, but we re-drive
    // its initialize() ourselves per iteration to control the loop cleanly.
    const auto P0_f = to_float(P0_d);
    const auto x0_f = to_float(x0_d);

    UKFCore::SRUKFSmoother<OD_STATE_DIM, OD_OBS_DIM> smoother(model);
    smoother.initialize(x0_f, P0_f);
    // Innovation gate + reject flag are enforced above to equal NLF's
    // built-in defaults (25.0, false), so the SRUKFSmoother's inaccessible
    // internal SRUKF and the monitor SRUKF below are guaranteed to run
    // with identical thresholds. No handoff needed.

    PassResult result;
    result.t_utc.reserve(input.observations.size() + 1);
    result.innovation_hz.reserve(input.observations.size());
    result.nis.reserve(input.observations.size());

    // Also drive a companion SRUKF alongside the smoother so we can capture
    // the NIS and innovation per epoch (SRUKFSmoother's SRUKF is private).
    UKFCore::SRUKF<OD_STATE_DIM, OD_OBS_DIM> mon(model);
    mon.initialize(x0_f, P0_f);
    mon.setInnovationGateChi2(static_cast<float>(cfg.innovation_gate_chi2));
    mon.setRejectOutliers(cfg.reject_outliers);

    // Forward filter helper: iterate observations, drive both smoother and
    // monitor SRUKF, capture per-epoch NIS and innovation.
    auto forward_pass = [&](double& loglik_out) {
        loglik_out = 0.0;
        result.t_utc.clear();
        result.t_utc.push_back(input.t_ref_utc);
        result.innovation_hz.clear();
        result.nis.clear();

        float t_prev_sec = 0.0f;
        model.set_t_prev_sec(t_prev_sec);

        for (const auto& obs : input.observations) {
            const double t_sec_d = seconds_between(input.t_ref_utc, obs.t_utc);
            const float  t_sec   = static_cast<float>(t_sec_d);

            // Predict then update on the smoother.
            model.set_t_prev_sec(t_prev_sec);
            OrbitStateSpaceModel::State u = OrbitStateSpaceModel::State::Zero();
            OrbitStateSpaceModel::Observation y;
            y(0) = static_cast<float>(obs.f_hz);
            smoother.step(t_sec, y, u);

            // Drive the monitor filter identically so we get NIS/innovation.
            model.set_t_prev_sec(t_prev_sec);   // reset -- mon uses same model
            mon.predict(t_sec, u);
            // Predicted h(x_pred) BEFORE update (for innovation).
            const OrbitStateSpaceModel::State x_pred = mon.getState();
            StateVecD x_pred_d = to_double(x_pred);
            const double f_pred = predict_doppler_hz(
                x_pred_d, obs.t_utc, cfg.f_transmit_hz, cfg.station,
                input.t_ref_utc);
            const double innov = obs.f_hz - f_pred;
            mon.update(t_sec, y);

            // NIS from the monitor SRUKF.
            const double nis = static_cast<double>(mon.getLastNIS());
            result.innovation_hz.push_back(innov);
            result.nis.push_back(nis);
            result.t_utc.push_back(obs.t_utc);

            // Log-likelihood contribution:
            //   l_k = -0.5 * ( log(2 pi R_eff) + nis )
            // where R_eff = R (1x1). We use cfg.sigma_R_hz^2.
            const double R_eff = cfg.sigma_R_hz * cfg.sigma_R_hz;
            loglik_out += -0.5 * (std::log(2.0 * std::numbers::pi * R_eff) + nis);

            t_prev_sec = t_sec;
        }
    };

    double loglik_prev = 0.0, loglik_curr = 0.0;
    forward_pass(loglik_curr);

    // Mode A stops here: no smoothing.
    if (mode == Mode::A_FilterOnly) {
        result.iterations_used = 0;
        result.final_loglik    = loglik_curr;
        result.converged       = true;
        result.x_filtered.reserve(smoother.size());
        result.P_filtered.reserve(smoother.size());
        for (int k = 0; k < smoother.size(); ++k) {
            result.x_filtered.push_back(to_double(smoother.filtered_state(k)));
            result.P_filtered.push_back(to_double(smoother.filtered_covariance(k)));
        }
        return result;
    }

    // Modes B/C/D: at least one backward RTS pass.
    smoother.smooth(0);
    std::vector<StateVecD> xs_prev, xs_curr;
    std::vector<StateMatD> Ps_prev, Ps_curr;
    xs_curr.reserve(smoother.size());
    Ps_curr.reserve(smoother.size());
    for (int k = 0; k < smoother.size(); ++k) {
        xs_curr.push_back(to_double(smoother.smoothed_state(k)));
        Ps_curr.push_back(to_double(smoother.smoothed_covariance(k)));
    }

    int iter_used = 1;   // one forward + one backward = "iteration 1"
    bool converged = true;

    if (mode == Mode::D_Iterated) {
        for (int it = 2; it <= it_cfg.max_iterations; ++it) {
            xs_prev = xs_curr;
            Ps_prev = Ps_curr;
            loglik_prev = loglik_curr;

            // Re-drive the smoother from the smoothed initial condition,
            // KEEPING the original P0 -- this matches SRUKFSmoother::smooth's
            // documented iteration recipe.
            const OrbitStateSpaceModel::State x_smooth_init_f =
                to_float(xs_prev.front());
            smoother.initialize(x_smooth_init_f, P0_f);
            mon.initialize(x_smooth_init_f, P0_f);
            mon.setInnovationGateChi2(static_cast<float>(cfg.innovation_gate_chi2));
            mon.setRejectOutliers(cfg.reject_outliers);
            forward_pass(loglik_curr);
            smoother.smooth(0);

            xs_curr.clear();
            Ps_curr.clear();
            for (int k = 0; k < smoother.size(); ++k) {
                xs_curr.push_back(to_double(smoother.smoothed_state(k)));
                Ps_curr.push_back(to_double(smoother.smoothed_covariance(k)));
            }
            iter_used = it;

            // Dual convergence check: state Mahalanobis change AND log-lik
            // change. Halt on EITHER trigger (belt-and-braces per docs §5.5).
            const double dx = smoothed_state_change_maha(xs_prev, Ps_prev, xs_curr);
            const double dl = std::abs(loglik_curr - loglik_prev);
            if (dx < it_cfg.tol_state_maha || dl < it_cfg.tol_loglik) {
                converged = true;
                break;
            }
            converged = false;   // will flip back true only if a criterion trips
        }
    }

    // Populate result arrays from the FINAL smoother state.
    result.x_filtered.reserve(smoother.size());
    result.P_filtered.reserve(smoother.size());
    result.x_smoothed.reserve(smoother.size());
    result.P_smoothed.reserve(smoother.size());
    for (int k = 0; k < smoother.size(); ++k) {
        result.x_filtered.push_back(to_double(smoother.filtered_state(k)));
        result.P_filtered.push_back(to_double(smoother.filtered_covariance(k)));
        result.x_smoothed.push_back(to_double(smoother.smoothed_state(k)));
        result.P_smoothed.push_back(to_double(smoother.smoothed_covariance(k)));
    }

    result.iterations_used = iter_used;
    result.final_loglik    = loglik_curr;
    result.converged       = converged;

    if (mode == Mode::C_SmoothToAOS && !result.x_smoothed.empty()) {
        result.smoothed_at_aos   = result.x_smoothed.front();
        result.P_smoothed_at_aos = result.P_smoothed.front();
    }
    return result;
}

} // namespace ve::od
