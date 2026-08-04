// doppler_measurement.cpp -- Implementation of the relativistic-Doppler
// measurement model and its analytical Jacobian.
//
// Station kinematics duplicate the same WGS84 + GMST logic used by
// ve::Observer (src/observer.cpp). We do not call ve::Observer directly
// because its TimePoint -> ECI helpers use double precision and are keyed
// to the tracker's specific TimePoint conventions; we want to be certain
// that any GMST/rotation deviation is either identical to the tracker's or
// documented here. If the tracker's Observer implementation changes, that
// change should be mirrored here (see unit test T2 which checks kinematic
// consistency numerically).

#include "od/doppler_measurement.hpp"

#include <cmath>

namespace ve::od {

namespace {

// Rebuild the station's Greenwich sidereal time from a UTC TimePoint using
// exactly the formula ve::Observer uses. Kept as a local helper so the
// dependency on ve::Observer's private method is not a public coupling.
double station_gst(const ve::TimePoint& t) {
    return ve::getGMST(t);   // types.hpp inline helper
}

ve::Vector3 station_position_teme(const ve::Geodetic& g, const ve::TimePoint& t) {
    const double lat_rad = g.lat_deg * ve::DEG2RAD;
    const double lon_rad = g.lon_deg * ve::DEG2RAD;
    // WGS84 ellipsoid (matches src/observer.cpp exactly).
    constexpr double a  = 6378.137;                // km
    constexpr double f  = 1.0 / 298.257223563;
    constexpr double e2 = 2.0 * f - f * f;
    const double sinLat = std::sin(lat_rad);
    const double cosLat = std::cos(lat_rad);
    const double N = a / std::sqrt(1.0 - e2 * sinLat * sinLat);
    const double x_ecf = (N + g.alt_km) * cosLat * std::cos(lon_rad);
    const double y_ecf = (N + g.alt_km) * cosLat * std::sin(lon_rad);
    const double z_ecf = (N * (1.0 - e2) + g.alt_km) * sinLat;
    const double theta = station_gst(t);
    return { x_ecf * std::cos(theta) - y_ecf * std::sin(theta),
             x_ecf * std::sin(theta) + y_ecf * std::cos(theta),
             z_ecf };
}

// Station inertial velocity in TEME: v = omega x r (rigid rotation of the
// ellipsoid). Same convention as ve::Observer::getVelocityECI.
ve::Vector3 station_velocity_teme(const ve::Geodetic& g, const ve::TimePoint& t) {
    const ve::Vector3 p = station_position_teme(g, t);
    // Earth rotation rate = ve::EARTH_ROTATION_RATE (rad/s).
    const double omega = ve::EARTH_ROTATION_RATE;
    return { -omega * p.y, omega * p.x, 0.0 };
}

} // namespace

DopplerPrediction predict_doppler(
    const StateVecD& x,
    const ve::TimePoint& t,
    double f_transmit_hz,
    const ve::Geodetic& station,
    const ve::TimePoint& t_ref)
{
    const ve::Vector3 r_sat = pos_from_state(x);
    const ve::Vector3 v_sat = vel_from_state(x);
    const ve::Vector3 r_stn = station_position_teme(station, t);
    const ve::Vector3 v_stn = station_velocity_teme(station, t);

    // First-order light-time (retardation) correction (audit fix).
    //
    // The photons arriving at t = t_obs were emitted at t_emit = t_obs - tau
    // where tau = |rho| / c is the one-way light travel time. To first order
    // the satellite position at emission is
    //     r_sat_ret = r_sat(t_obs) - v_sat(t_obs) * tau
    // and the receiver-side geometry (r_stn, v_stn) is evaluated at t_obs.
    // For LEO tau ~ 17 ms and this shifts rho by ~130 m along-track, giving
    // a coherent ~1 Hz within-pass Doppler bias that we now remove.
    //
    // The analytical Jacobian doppler_jacobian() below is derived at the
    // INSTANTANEOUS limit tau -> 0; treating the retardation as a constant
    // introduces a relative Jacobian error of O(v/c) ~ 2.5e-5 for LEO,
    // well below the T3 finite-difference tolerance of 5e-4. If a future
    // caller needs the exact linearisation (e.g. an EKF at GEO where
    // beta is larger), the Jacobian must be rederived with tau treated as
    // a state-dependent quantity.
    const ve::Vector3 rho_inst = r_sat - r_stn;
    const double s_inst = rho_inst.magnitude();
    const double tau_sec = s_inst / C_KM_S;
    const ve::Vector3 r_sat_ret = r_sat - v_sat * tau_sec;

    const ve::Vector3 rho = r_sat_ret - r_stn;
    const double s = rho.magnitude();
    const ve::Vector3 rho_hat = (s > 0.0) ? (rho * (1.0 / s)) : ve::Vector3{0,0,0};
    const ve::Vector3 v_rel = v_sat - v_stn;

    const double beta_los = v_rel.dot(rho_hat) / C_KM_S;   // dimensionless
    const double beta_sq  = v_rel.dot(v_rel) / (C_KM_S * C_KM_S);
    // Classical range-rate (kept for diagnostics; NOT what f_R uses).
    const double rho_dot = (s > 0.0) ? v_rel.dot(rho) / s : 0.0;

    // Special-relativistic Doppler, one-way, source-at-satellite.
    //
    // Standard form (see Rindler §3.7): f_R = f_T * sqrt(1 - beta^2) /
    //   (1 - beta_light) where beta_light = (v_source · unit_light_direction)/c.
    // Light travels FROM satellite TO station, i.e. in direction (-rho_hat),
    // so beta_light = v_rel · (-rho_hat) / c = -beta_los (with our
    // rho_hat = station-to-satellite convention). Substituting gives:
    //
    //     f_R = f_T * sqrt(1 - beta^2) / (1 + beta_los)
    //
    // Sanity check: for pure recession (beta_los > 0), denom > 1, so f_R < f_T
    // (redshift), as expected. This is the sign that was caught by unit test
    // T4 (limit-case check against classical Doppler); the earlier draft used
    // (1 - beta_los), which is a common sign error.
    const double denom = 1.0 + beta_los;
    const double safe_denom = (std::abs(denom) < 1e-15) ? 1e-15 : denom;
    const double one_minus_bsq = std::max(0.0, 1.0 - beta_sq);
    const double f_sr = f_transmit_hz * std::sqrt(one_minus_bsq) / safe_denom;

    const double dt_sec = seconds_between(t_ref, t);
    const double f_bias = x(idx::BC) + x(idx::BD) * dt_sec;

    DopplerPrediction out;
    out.f_hz            = f_sr + f_bias;
    out.range_km        = s;
    out.range_rate_km_s = rho_dot;
    out.beta_los        = beta_los;
    out.beta_sq         = beta_sq;
    return out;
}

double predict_doppler_hz(
    const StateVecD& x,
    const ve::TimePoint& t,
    double f_transmit_hz,
    const ve::Geodetic& station,
    const ve::TimePoint& t_ref)
{
    return predict_doppler(x, t, f_transmit_hz, station, t_ref).f_hz;
}

Eigen::Matrix<double, 1, OD_STATE_DIM> doppler_jacobian(
    const StateVecD& x,
    const ve::TimePoint& t,
    double f_transmit_hz,
    const ve::Geodetic& station,
    const ve::TimePoint& t_ref)
{
    // Rebuild everything the prediction uses; keep names matching the
    // derivation comment in the header.
    const ve::Vector3 r_sat = pos_from_state(x);
    const ve::Vector3 v_sat = vel_from_state(x);
    const ve::Vector3 r_stn = station_position_teme(station, t);
    const ve::Vector3 v_stn = station_velocity_teme(station, t);
    const ve::Vector3 rho   = r_sat - r_stn;
    const double s = rho.magnitude();
    const ve::Vector3 rho_hat = (s > 0.0) ? (rho * (1.0 / s)) : ve::Vector3{0,0,0};
    const ve::Vector3 v_rel = v_sat - v_stn;

    const double c   = C_KM_S;
    const double g   = v_rel.dot(rho_hat) / c;     // beta_los
    const double b2  = v_rel.dot(v_rel) / (c * c); // beta^2
    const double one_minus_bsq = std::max(0.0, 1.0 - b2);
    const double sqrt_1m_b2 = std::sqrt(one_minus_bsq);
    // Denominator is (1 + g) with our rho_hat = station-to-satellite
    // convention (see predict_doppler for the sign derivation and the T4
    // test result that caught the earlier (1 - g) mistake).
    const double one_plus_g = 1.0 + g;
    const double safe_opg   = (std::abs(one_plus_g) < 1e-15) ? 1e-15 : one_plus_g;
    const double A = sqrt_1m_b2 / safe_opg;                 // f_R_dyn / f_T (no bias)

    // f_R_dyn = f_T * sqrt(1 - b2) / (1 + g). Then
    //   d f_R_dyn / d(...) = f_T * [ (d sqrt(1-b2)/d(...))/(1+g)
    //                              -  sqrt(1-b2) * (dg/d(...))/(1+g)^2 ]
    // Coefficient of dg/d(...) is:  -f_T * sqrt(1-b2) / (1+g)^2 = -f_T*A/(1+g)
    const double K_g = -f_transmit_hz * A / safe_opg;   // multiplies dg/d(state)

    // d g / d r_i = (v_rel_i - g * rho_hat_i) / (c * s)   (v_rel component perp
    //    to rho_hat, divided by (c*s)); see derivation in header.
    Eigen::Matrix<double, 1, OD_STATE_DIM> J = Eigen::Matrix<double, 1, OD_STATE_DIM>::Zero();
    if (s > 0.0) {
        const ve::Vector3 dg_dr = (v_rel - rho_hat * g * c) * (1.0 / (c * s));
        // Note: v_rel - g*c*rho_hat gives the vector; divide by (c*s).
        // Cross-check: dg/dr . rho_hat should be zero.
        J(0, idx::RX) = K_g * dg_dr.x;
        J(0, idx::RY) = K_g * dg_dr.y;
        J(0, idx::RZ) = K_g * dg_dr.z;

        // d g / d v_i = rho_hat_i / c
        // Plus the sqrt(1-b2)-derivative term: d/dv_i sqrt(1-b2) = -v_rel_i/(c^2 sqrt(1-b2))
        // Multiplied by f_T/(1+g).
        const double vel_prefactor_b2 =
            -f_transmit_hz / (c * c * std::max(sqrt_1m_b2, 1e-15)) / safe_opg;
        J(0, idx::VX) = K_g * (rho_hat.x / c) + vel_prefactor_b2 * v_rel.x;
        J(0, idx::VY) = K_g * (rho_hat.y / c) + vel_prefactor_b2 * v_rel.y;
        J(0, idx::VZ) = K_g * (rho_hat.z / c) + vel_prefactor_b2 * v_rel.z;
    }

    J(0, idx::BC) = 1.0;
    J(0, idx::BD) = seconds_between(t_ref, t);
    return J;
}

} // namespace ve::od
