// od_types.hpp -- Shared value types, constants, and small helpers for the
// orbit-determination (OD) subsystem.
//
// Newton Architect note: units and frames are load-bearing here. Every field
// carries a unit in its comment; every ECI-labelled vector is in the SAME
// TEME-based frame the existing ForceModel and NumericalPropagator use, per
// docs/orbit_determination.md §F1. Any user of these types who intends a
// different frame must document that at the callsite.
//
// The 8-state augmented vector layout used by the SRUKF is:
//     x[0..2] = r_x, r_y, r_z   (position, km, ECI-TEME)
//     x[3..5] = v_x, v_y, v_z   (velocity, km/s, ECI-TEME)
//     x[6]    = b_c              (ground-station oscillator bias, Hz)
//     x[7]    = b_dot            (oscillator drift, Hz/s)
//
// See docs/orbit_determination.md §4 for the rationale.

#pragma once

#include "types.hpp"       // ve::Vector3, ve::Geodetic, ve::TimePoint, constants
#include "force_model.hpp" // ve::ForceParams (needed by FilterConfig below)
#include <Eigen/Dense>

#include <vector>
#include <cstdint>
#include <limits>

namespace ve::od {

// ---------------------------------------------------------------------------
// Constants used by the OD subsystem.
// ---------------------------------------------------------------------------

// Speed of light in km/s, exact per SI 2019.
inline constexpr double C_KM_S = 299792.458;

// Filter state size and observation size (fixed at compile time for NLF's
// template SRUKF<NX, NY>).
inline constexpr int OD_STATE_DIM = 8;
inline constexpr int OD_OBS_DIM   = 1;

// Named indices into the 8-vector.
namespace idx {
    inline constexpr int RX = 0, RY = 1, RZ = 2;
    inline constexpr int VX = 3, VY = 4, VZ = 5;
    inline constexpr int BC = 6;   // oscillator bias      [Hz]
    inline constexpr int BD = 7;   // oscillator drift     [Hz/s]
}

// ---------------------------------------------------------------------------
// Eigen type aliases in the OD subsystem's double precision.
// The NLF SRUKF is single-precision internally; conversion happens at the
// module boundary in orbit_ssm.hpp.
// ---------------------------------------------------------------------------

using StateVecF = Eigen::Matrix<float,  OD_STATE_DIM, 1>;
using StateMatF = Eigen::Matrix<float,  OD_STATE_DIM, OD_STATE_DIM>;
using ObsVecF   = Eigen::Matrix<float,  OD_OBS_DIM,   1>;
using ObsMatF   = Eigen::Matrix<float,  OD_OBS_DIM,   OD_OBS_DIM>;

using StateVecD = Eigen::Matrix<double, OD_STATE_DIM, 1>;
using StateMatD = Eigen::Matrix<double, OD_STATE_DIM, OD_STATE_DIM>;

// ---------------------------------------------------------------------------
// Small helpers for r/v <-> 8-vector packing (keeps callsites readable).
// ---------------------------------------------------------------------------

inline StateVecD make_state(const ve::Vector3& r_km, const ve::Vector3& v_km_s,
                            double bc_hz = 0.0, double bd_hz_s = 0.0) {
    StateVecD x;
    x << r_km.x, r_km.y, r_km.z,
         v_km_s.x, v_km_s.y, v_km_s.z,
         bc_hz, bd_hz_s;
    return x;
}

inline ve::Vector3 pos_from_state(const StateVecD& x) {
    return {x(idx::RX), x(idx::RY), x(idx::RZ)};
}
inline ve::Vector3 vel_from_state(const StateVecD& x) {
    return {x(idx::VX), x(idx::VY), x(idx::VZ)};
}

// ---------------------------------------------------------------------------
// Radial / Along-track / Cross-track (RSW) basis at an instantaneous (r, v).
// Standard orbital-mechanics local frame:
//     R = r_hat                                    (radial, outward from Earth)
//     W = (r x v) / |r x v|                        (cross-track, orbit normal)
//     S = W x R                                    (along-track, in direction of v)
//
// Returns the 3x3 rotation matrix whose COLUMNS are (R, S, W); i.e. it maps
// an RSW-basis vector to the ECI basis:  v_eci = R_eci_from_rsw * v_rsw.
// The inverse is transpose. Used in docs/orbit_determination.md §7.2 to
// build an anisotropic prior P0 aligned with the along-track error.
// ---------------------------------------------------------------------------

inline Eigen::Matrix3d rsw_to_eci_basis(const ve::Vector3& r, const ve::Vector3& v) {
    ve::Vector3 R = r.normalize();
    ve::Vector3 W_raw = r.cross(v);
    ve::Vector3 W = W_raw.normalize();
    ve::Vector3 S = W.cross(R);
    Eigen::Matrix3d M;
    M << R.x, S.x, W.x,
         R.y, S.y, W.y,
         R.z, S.z, W.z;
    return M;
}

// Build the 8x8 prior P0 from RSW-frame position/velocity 1-sigmas plus
// oscillator bias/drift 1-sigmas. Everything expressed in the SAME units
// as the state vector (km, km/s, Hz, Hz/s).
struct RswPriorSigmas {
    double sigma_r_radial = 0.5;   // km
    double sigma_r_along  = 5.0;   // km
    double sigma_r_cross  = 1.0;   // km
    double sigma_v_radial = 0.5e-3; // km/s  (i.e. 0.5 m/s)
    double sigma_v_along  = 5.0e-3;
    double sigma_v_cross  = 1.0e-3;
    double sigma_bc       = 50.0;  // Hz
    double sigma_bd       = 1.0;   // Hz/s
};

// Rotate the diagonal RSW-frame P into ECI, and pad with bc/bd blocks.
// The RSW-frame covariance is diag(sigma_r_*^2) for position, diag(sigma_v_*^2)
// for velocity, both in R/S/W order. Rotation is C = M P_rsw M^T where M is
// the RSW->ECI basis matrix.
StateMatD build_prior_covariance(const ve::Vector3& r_at_epoch,
                                 const ve::Vector3& v_at_epoch,
                                 const RswPriorSigmas& sigmas);

// ---------------------------------------------------------------------------
// Doppler observation record and configuration structs used by the driver.
// ---------------------------------------------------------------------------

struct DopplerObservation {
    ve::TimePoint t_utc;   // absolute UTC epoch of the measurement
    double        f_hz;    // measured received frequency, Hz
};

struct FilterConfig {
    // Transmitter (satellite) reference frequency in Hz. Required (no default;
    // this is a physical property of the target link, not a knob).
    double f_transmit_hz = 0.0;

    // Ground station geodetic coordinates.
    ve::Geodetic station{0.0, 0.0, 0.0};

    // Prior covariance in RSW frame (see build_prior_covariance).
    RswPriorSigmas prior_rsw{};

    // Diagonal process-noise 1-sigmas -- see docs §7.3.
    double sigma_process_accel = 1.0e-6;    // km/s^2, converted to Q_vel = sigma^2 * dt^2 I
    double sigma_process_bc    = 1.0;       // Hz per sqrt(second)
    double sigma_process_bd    = 0.05;      // Hz/s per sqrt(second)

    // Measurement noise. R = sigma_R^2, in Hz^2.
    double sigma_R_hz = 5.0;

    // Force-model parameters (default: same defaults as the rest of the tracker,
    // EGM96 10x10 + Sun + Moon + drag + SRP).
    ve::ForceParams force_params{};

    // Innovation-gate chi-squared threshold and outlier-rejection flag.
    //
    // NEWTON note (audit fix): the underlying NLF SRUKFSmoother does not
    // expose its internal SRUKF, so the driver in od_smoother.cpp cannot
    // wire these settings into the smoother's actual filter -- only the
    // "monitor" SRUKF used for NIS reporting would receive them. Rather
    // than silently discard caller intent, od::run enforces that these
    // fields match NLF's built-in defaults (25.0 / false). Any other
    // value causes od::run to throw. If per-mode gate control is needed,
    // NLF's SRUKFSmoother must be patched (or subclassed with a public
    // accessor) to expose its internal SRUKF.
    static constexpr double NLF_DEFAULT_INNOVATION_GATE_CHI2 = 25.0;
    double innovation_gate_chi2 = NLF_DEFAULT_INNOVATION_GATE_CHI2;
    bool   reject_outliers      = false;

    // Reference time for the oscillator drift term: b(t) = b_c + b_dot*(t - t_ref).
    // Set to the pass AOS by the driver; exposed here for tests.
    ve::TimePoint t_ref_utc{};
};

struct IterationConfig {
    int    max_iterations = 20;      // hard cap (Newton: never unbounded)
    double tol_state_maha = 1.0e-6;  // Mahalanobis-norm state-change threshold
    double tol_loglik     = 1.0e-4;  // log-likelihood-change threshold
};

struct PassResult {
    std::vector<ve::TimePoint>      t_utc;
    std::vector<StateVecD>          x_filtered;
    std::vector<StateMatD>          P_filtered;
    std::vector<StateVecD>          x_smoothed;   // empty in Mode A
    std::vector<StateMatD>          P_smoothed;   // empty in Mode A
    std::vector<double>             innovation_hz;
    std::vector<double>             nis;
    int      iterations_used = 0;
    double   final_loglik    = 0.0;
    bool     converged       = false;
    // If Mode C was requested, this echoes the smoothed state and covariance
    // at the interval START (AOS) for convenience.
    StateVecD smoothed_at_aos = StateVecD::Zero();
    StateMatD P_smoothed_at_aos = StateMatD::Zero();
};

// Convert TimePoint difference to seconds as a double. Kept in one place so
// unit-conversion errors have exactly one file to audit.
inline double seconds_between(const ve::TimePoint& a, const ve::TimePoint& b) {
    using D = std::chrono::duration<double>;
    return std::chrono::duration_cast<D>(b - a).count();
}

// -----------------------------------------------------------------------------
// State sanity diagnostics -- used by the coordinate-frame guardrail (T10).
//
// From (r, v) in what is CLAIMED to be an inertial (ECI/TEME) frame, compute
// the two-body orbital elements a (semi-major axis, km) and e (eccentricity).
// A physically plausible LEO/MEO/GEO satellite in ECI gives a in a well-known
// range and e < 1. If the caller has accidentally supplied an ECEF-frame
// state (velocity relative to a rotating Earth), the computed a and e come
// out wildly wrong (typically e >> 1 or a of the wrong sign).
//
// Returns is_plausible = false if:
//   - specific energy is not negative (unbound orbit -- wrong for a satellite)
//   - semi-major axis outside [6500, 500000] km
//   - eccentricity >= 1 (parabolic or hyperbolic)
// -----------------------------------------------------------------------------
struct OrbitDiagnostics {
    double semi_major_axis_km;
    double eccentricity;
    double specific_energy_km2_s2;
    bool   is_plausible;
    const char* reason;   // static string; empty if is_plausible
};

inline OrbitDiagnostics diagnose_state(const ve::Vector3& r, const ve::Vector3& v) {
    OrbitDiagnostics d{};
    const double r_mag = r.magnitude();
    const double v_mag = v.magnitude();
    const double mu = ve::EARTH_MU;   // km^3/s^2
    const double energy = 0.5 * v_mag * v_mag - mu / r_mag;
    d.specific_energy_km2_s2 = energy;
    if (energy >= 0.0) {
        d.semi_major_axis_km = std::numeric_limits<double>::infinity();
        d.eccentricity = std::numeric_limits<double>::infinity();
        d.is_plausible = false;
        d.reason = "non-negative specific energy (unbound trajectory)";
        return d;
    }
    const double a = -mu / (2.0 * energy);
    // Eccentricity vector: e = (v x h)/mu - r_hat, where h = r x v.
    const ve::Vector3 h = r.cross(v);
    const ve::Vector3 rhat = r.normalize();
    const double coef = 1.0 / mu;
    const ve::Vector3 vxh = v.cross(h);
    const ve::Vector3 ecc_vec = { vxh.x * coef - rhat.x,
                                  vxh.y * coef - rhat.y,
                                  vxh.z * coef - rhat.z };
    const double e = ecc_vec.magnitude();
    d.semi_major_axis_km = a;
    d.eccentricity = e;
    d.is_plausible = true;
    d.reason = "";
    if (a < 6500.0 || a > 500000.0) {
        d.is_plausible = false;
        d.reason = "semi-major axis outside plausible Earth-satellite range";
    } else if (e >= 1.0) {
        d.is_plausible = false;
        d.reason = "eccentricity >= 1 (parabolic or hyperbolic)";
    }
    return d;
}

} // namespace ve::od
