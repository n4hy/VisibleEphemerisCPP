// od_smoother.hpp -- Orbit-determination driver: Modes A / B / C / D.
//
// Mode A -- forward SRUKF only, no smoothing pass.
// Mode B -- forward SRUKF followed by a single backward SRUKF RTS pass.
// Mode C -- Mode B, plus PassResult.smoothed_at_aos is set (the smoothed
//           state at the pass's first epoch, useful as the corrected prior
//           for the next pass).
// Mode D -- iterated: forward, backward, re-forward (from the smoothed
//           initial condition, ORIGINAL P0), backward, ... until either the
//           state-Mahalanobis change or the log-likelihood change drops
//           below the configured thresholds, or max_iterations is reached.
//
// See docs/orbit_determination.md §5 for the mathematical statement and the
// justification for the two convergence criteria. Convergence is checked
// AGAINST the previous iteration's smoothed trajectory; the first iteration
// (i=1) is compared against the initial (i=0) forward pass.

#pragma once

#include "od/od_types.hpp"
#include "force_model.hpp"

namespace ve::od {

enum class Mode { A_FilterOnly, B_ForwardSmooth, C_SmoothToAOS, D_Iterated };

// The input to the driver.
struct PassInput {
    // Initial state at the pass reference epoch (typically the AOS state
    // produced by propagating a TLE forward with the deterministic
    // NumericalPropagator).
    StateVecD x0_at_t_ref;

    // Reference epoch (UTC). All observation timestamps are measured
    // relative to this. Typically the pass AOS time.
    ve::TimePoint t_ref_utc;

    // Julian Date corresponding to t_ref_utc. Required by the force model
    // (which is time-varying via Sun/Moon positions).
    double jd_at_t_ref;

    // Doppler observations, in chronological order. Timestamps must be >=
    // t_ref_utc (the driver rejects earlier observations with a runtime
    // error rather than silently reordering).
    std::vector<DopplerObservation> observations;
};

// Run the OD subsystem in the requested mode. The ForceModel is a caller-
// owned reference so its coefficient tables are not rebuilt per call.
//
// Newton note: the returned PassResult tags every algorithmic result with
// its epistemic status via named fields (filtered vs smoothed, iterations
// used, converged flag, final log-likelihood, per-epoch NIS). Callers
// presenting numbers to a user are expected to state R, Q, and the mode
// used alongside them (see docs §7.4, §8).
PassResult run(Mode mode,
               const PassInput& input,
               const FilterConfig& cfg,
               const IterationConfig& it_cfg,
               const ve::ForceModel& fm);

} // namespace ve::od
