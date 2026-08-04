// doppler_measurement.hpp -- One-way downlink Doppler measurement model for
// the OD subsystem.
//
// Given an 8-state x = [r; v; b_c; b_dot] in TEME (km, km/s, Hz, Hz/s), a
// ground station with a known geodetic location, a transmitter reference
// frequency f_T, and a measurement epoch t, this computes the predicted
// received frequency:
//
//     f_R = f_T * sqrt(1 - beta^2) / (1 + beta_los)
//                                      +  b_c
//                                      +  b_dot * (t - t_ref)
//
// where
//     beta_vec = (v_sat_teme - v_station_teme) / c
//     rho_hat  = (r_sat_teme - r_station_teme) / |r_sat_teme - r_station_teme|
//     beta_los = beta_vec . rho_hat        (component along station-to-sat LOS;
//                                           beta_los > 0 for recession)
//     beta^2   = |beta_vec|^2              (full 3-D speed / c, squared)
//
// Sign of the denominator: the standard SR formula is
// f_R = f_T sqrt(1-β²)/(1 - β_light) where β_light is the source-velocity
// component ALONG THE DIRECTION LIGHT TRAVELS (source→observer). Light
// travels in the -rho_hat direction, so β_light = -beta_los, giving
// (1 + beta_los) here. Verified by unit test T4 (limit-case reduction to
// classical Doppler).
//
// The formula is the standard special-relativistic Doppler shift
// (Rindler, Relativity §3.7). For LEO |beta| ~ 2.6e-5 so the sqrt(1 - beta^2)
// factor differs from 1 by ~3.4e-10 (~0.5 Hz at 1.6 GHz Iridium downlink).
// It is included because the OD subsystem is explicitly built to be
// dimensionally honest about the observations it uses.
//
// The station kinematics (position, velocity) use the SAME TEME/GMST-based
// convention as ve::Observer -- see src/observer.cpp -- so this model is
// self-consistent with the propagator's frame per docs §F1.
//
// Newton note: no troposphere/ionosphere modelling in v0. Their contribution
// aliases into either R inflation or the estimated oscillator bias; that is
// stated in docs §7.4 and must be echoed in every result presentation.

#pragma once

#include "types.hpp"
#include "od/od_types.hpp"

namespace ve::od {

// All double-precision. Callers converting from float SRUKF state should
// widen at the call site (orbit_ssm.hpp handles this).
struct DopplerPrediction {
    double f_hz;             // predicted received frequency [Hz]
    double range_km;         // ||r_sat - r_station|| [km]
    double range_rate_km_s;  // classical d/dt ||r_sat - r_station||, radial
    double beta_los;         // beta_vec . rho_hat  (dimensionless)
    double beta_sq;          // |beta_vec|^2         (dimensionless)
};

// Full prediction with diagnostic breakdown. `t` is the measurement UTC
// epoch (used to compute station ECI position/velocity and the oscillator
// drift term). `t_ref` is the reference epoch for the oscillator drift term.
DopplerPrediction predict_doppler(
    const StateVecD& x,
    const ve::TimePoint& t,
    double f_transmit_hz,
    const ve::Geodetic& station,
    const ve::TimePoint& t_ref);

// Convenience overload returning only the predicted frequency.
double predict_doppler_hz(
    const StateVecD& x,
    const ve::TimePoint& t,
    double f_transmit_hz,
    const ve::Geodetic& station,
    const ve::TimePoint& t_ref);

// -----------------------------------------------------------------------------
// Analytical Jacobian dh/dx (row vector, 1x8), evaluated at (x, t). Not used
// by SRUKF (which needs no Jacobian by design) but provided for:
//   (a) unit test T3 -- finite-difference agreement to 1e-6 relative;
//   (b) fallback EKF path in a future v1.
//
// Derivation (Newton-annotated so reviewers can verify each step). This
// tracks the (1 + g) sign convention used in predict_doppler above, which
// arises because rho_hat = station -> satellite and light travels in
// direction -rho_hat (from source to observer). An earlier draft of this
// comment used (1 - g); that was a documentation regression and did not
// match the implementation.
//
//   Let rho = r - r_s(t),  s = ||rho||,   rho_hat = rho / s.
//   Let v_rel = v - v_s(t),  beta = v_rel / c.
//   Let g = beta . rho_hat  =  (v_rel . rho) / (c s)     [scalar]
//   Let b2 = beta . beta   =  |v_rel|^2 / c^2            [scalar]
//   f_R = f_T * sqrt(1 - b2) / (1 + g)  +  b_c  +  b_dot * (t - t_ref)
//
//   Only r, v, b_c, b_dot are state components. Station position and
//   velocity are functions of t only.
//
//   d f_R / d r_i   =  -f_T * (sqrt(1 - b2) / (1 + g)^2) * (d g / d r_i)
//   d f_R / d v_i   = f_T * [ (-beta_i / (c sqrt(1-b2))) / (1 + g)
//                             -  (sqrt(1-b2) / (1 + g)^2) * (d g / d v_i) ]
//   d f_R / d b_c   = 1
//   d f_R / d b_dot = (t - t_ref)         [seconds]
//
//   with
//   d g / d r_i = (v_rel_i / (c s)) - (v_rel . rho) rho_i / (c s^3)
//              = (v_rel_i - (v_rel . rho_hat) rho_hat_i) / (c s)
//              = (component of v_rel perpendicular to rho_hat)_i / (c s)
//   d g / d v_i = rho_hat_i / c
//
// The implementation (doppler_measurement.cpp) writes this via a common
// prefactor K_g = -f_T * sqrt(1-b2) / (1 + g)^2  which multiplies both
// d g / d r_i and d g / d v_i, plus an additional velocity-only term
// vel_prefactor_b2 = -f_T / (c^2 sqrt(1-b2) (1 + g)) * v_rel_i from
// differentiating sqrt(1 - b2) with respect to v_i.
//
// The Jacobian is single-scalar (Doppler is scalar) -> returned as a
// 1xOD_STATE_DIM Eigen row.
// -----------------------------------------------------------------------------

Eigen::Matrix<double, 1, OD_STATE_DIM> doppler_jacobian(
    const StateVecD& x,
    const ve::TimePoint& t,
    double f_transmit_hz,
    const ve::Geodetic& station,
    const ve::TimePoint& t_ref);

} // namespace ve::od
