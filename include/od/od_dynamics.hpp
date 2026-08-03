// od_dynamics.hpp -- Standalone sigma-point orbital integrator.
//
// Why not use NumericalPropagator directly? Two reasons:
//
//   1. NumericalPropagator is STATEFUL. It carries r0/v0 at TLE epoch and a
//      cached checkpoint. Sigma-point propagation for the SRUKF forks the
//      state along 17 different trajectories from a common start point,
//      then re-forks for the next epoch. Fitting that into the propagator's
//      cache is awkward and would corrupt its normal-mode usage.
//   2. We want the OD integrator to be usable without touching the
//      propagator's public interface (Newton: minimize invasive changes).
//
// So we ship a small RK4 that borrows only the ForceModel::acceleration()
// method (a pure function of jd, r, v). The typical filter dt is ~1 sec,
// for which RK4 truncation error on LEO dynamics is well below 1e-6 km per
// step -- an order of magnitude below any other error in the system.
// Unit test T8 (propagator round-trip) exercises this claim.

#pragma once

#include "force_model.hpp"
#include "types.hpp"

#include <utility>

namespace ve::od {

// Integrate (r, v) from time (jd_utc, t_ref_sec) to (jd_utc, t_end_sec)
// under the supplied ForceModel. Sub-steps to keep each RK4 step no larger
// than `max_step_sec` (default 0.5 s). Returns the state at t_end_sec.
//
// Newton note: the ForceModel is stateless (it caches nothing between calls
// -- only its coefficient tables), so the same instance can be used by many
// sigma-point trajectories in the same predict step.
//
// jd_at_t_ref is the Julian Date corresponding to the local time-zero
// (t_ref_sec = 0). Time advances jd_at_t_ref by (t - t_ref)/86400.
std::pair<ve::Vector3, ve::Vector3> integrate_rk4(
    const ve::ForceModel& fm,
    double jd_at_t_ref,
    double t_ref_sec,
    const ve::Vector3& r_start,
    const ve::Vector3& v_start,
    double t_end_sec,
    double max_step_sec = 0.5);

} // namespace ve::od
