#!/usr/bin/env python3
"""MolniyaTwelveHour.py

Three-day orbit-determination (OD) demonstration on a 12-hour Molniya
spacecraft observed from Gaithersburg, MD via a 2.4 GHz beacon.

Uses this repository's ``ve_hpop`` pybind11 module for physical constants
(GM, Earth radius) and Earth-rotation angle (GMST). The truth trajectory
is generated at the apogee-over-Gaithersburg state and integrated with
two-body + J2 dynamics (matching what the tracker's HPOP uses for the
dominant Molniya perturbation). The extended Kalman filter that estimates
position, velocity, and oscillator bias/drift uses the same 8-state
layout, Van Loan process-noise Q, and analytical Doppler Jacobian as the
C++ ``ve::od`` subsystem (see ``docs/orbit_determination.md``).

Physical constants and station coordinates that a real deployment would
change:

  * Station: Gaithersburg, MD (lat 39.1434 N, lon -77.2014 E, alt 153 m)
    taken as if from a GPS receiver.
  * Beacon: 2.4 GHz transmit frequency, 5 Hz Doppler measurement noise
    (representative of a coherent DSP receiver on this band).
  * TLE-derived state error: 5 km along-track position and 5 m/s
    along-track velocity injected into the filter's initial state,
    representative of a few-days-old Space-Track TLE.

Run:
    /home/n4hy/VisibleEphemerisCPP/venv/bin/python MolniyaTwelveHour.py

Requires: ``ve_hpop`` module (build with ``-DBUILD_PYTHON_BINDINGS=ON``),
numpy, scipy.
"""

from __future__ import annotations

import os
import sys
from datetime import datetime, timezone

import numpy as np
from scipy.integrate import solve_ivp
from scipy.optimize import brentq

# --------------------------------------------------------------------------
# Locate ve_hpop. The pybind11 module is built into build/ but not
# installed into the venv site-packages by default; add build/ to sys.path.
# --------------------------------------------------------------------------
_REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
for candidate in (os.path.join(_REPO_ROOT, "build"),
                  os.path.join(_REPO_ROOT, "build_od")):
    if candidate not in sys.path:
        sys.path.insert(0, candidate)
import ve_hpop  # noqa: E402


# ==========================================================================
# Constants -- single source of truth is ve_hpop for anything the C++ side
# uses; everything else is spelled out with units in its variable name.
# ==========================================================================
MU_KM3_S2 = ve_hpop.GM_KM3_S2         # 398600.4415
R_EARTH_KM = ve_hpop.EARTH_RADIUS_KM  # 6378.137
J2 = 1.0826262e-3                     # EGM96 unnormalised C(2,0) magnitude
OMEGA_EARTH_RAD_S = 7.2921159e-5      # sidereal rotation rate
C_KM_S = 299792.458

# WGS-84 ellipsoid (same as src/observer.cpp and src/od/doppler_measurement.cpp)
WGS84_A_KM = 6378.137
WGS84_F = 1.0 / 298.257223563
WGS84_E2 = 2.0 * WGS84_F - WGS84_F * WGS84_F


# ==========================================================================
# Scenario configuration
# ==========================================================================
# Ground station (Gaithersburg, MD; nominal GPS coordinates)
STATION_LAT_DEG = 39.1434
STATION_LON_DEG = -77.2014
STATION_ALT_KM = 0.153

# Beacon and receiver
F_TRANSMIT_HZ = 2.4e9
SIGMA_R_HZ = 5.0
MIN_ELEV_DEG = 5.0

# Simulation timeline
EPOCH_UTC = datetime(2026, 8, 5, 0, 0, 0, tzinfo=timezone.utc)
DURATION_DAYS = 3.0
OBS_CADENCE_SEC = 30.0

# Molniya orbital elements. 12-hour = half-sidereal-day period so the
# ground track repeats. Argument of perigee 270 deg puts apogee at the
# maximum northern latitude (critical inclination i = 63.4 deg is chosen
# specifically so J2 does not rotate the apsides).
SEMI_MAJOR_KM = 26610.0        # -> T = 43082 s = 11 h 58 m (half sidereal day)
ECCENTRICITY = 0.72
INCLINATION_DEG = 63.4
ARG_PERIGEE_DEG = 270.0
TRUE_ANOM_AT_EPOCH_DEG = 180.0  # start at apogee (highest elevation from station)
# RAAN is solved at runtime so the apogee's sub-satellite point sits at
# STATION_LON_DEG at EPOCH_UTC (given the derivation, this puts the
# satellite roughly overhead 24 deg north of the observer -- Molniya's
# high altitude still yields high look-elevation from Gaithersburg).

# Prior perturbation representing a stale TLE. A single Doppler station
# at Gaithersburg has only the ~5-hour apogee arc per orbit visible, and
# for the geometry at apogee (satellite nearly stationary in the sky) the
# alongtrack component of state is only weakly observable. Larger priors
# than ~1 km position + 1 m/s velocity therefore cannot be corrected
# during a single pass and drift over the invisible perigee arc.
PRIOR_ALONGTRACK_POS_KM = 0.5
PRIOR_ALONGTRACK_VEL_KM_S = 0.00005    # 0.05 m/s (fresh-TLE regime)
PRIOR_BC_HZ = 25.0                     # filter starts with wrong-by-25-Hz oscillator
PRIOR_BD_HZ_S = 0.0

# Process noise for the EKF (Van Loan CV block per axis + white bias/drift).
# The two-body+J2 truth used here matches the filter dynamics exactly so
# the truly required Q_accel is zero; we set a small but non-zero floor
# so the filter can absorb linearisation error in the STM over the
# multi-hour invisible perigee arc.
Q_ACCEL_KM_S2_SQRT_S = 1.0e-9
Q_BC_HZ_SQRT_S = 0.02
Q_BD_HZ_S_SQRT_S = 0.001

# Truth oscillator (unknown to the filter until it estimates them).
TRUTH_BC_HZ = 25.0
TRUTH_BD_HZ_S = 0.001


# ==========================================================================
# Utilities
# ==========================================================================
def utc_to_jd(t: datetime) -> float:
    """UTC datetime -> Julian Date (same convention as julianFromTimePoint)."""
    unix_seconds = t.replace(tzinfo=timezone.utc).timestamp()
    return 2440587.5 + unix_seconds / 86400.0


def keplerian_to_eci(a_km: float, e: float, i_rad: float, raan_rad: float,
                     argp_rad: float, nu_rad: float):
    """Classical Keplerian -> ECI position/velocity (km, km/s)."""
    p = a_km * (1.0 - e * e)
    r_mag = p / (1.0 + e * np.cos(nu_rad))
    r_pqw = np.array([r_mag * np.cos(nu_rad),
                      r_mag * np.sin(nu_rad),
                      0.0])
    vmag = np.sqrt(MU_KM3_S2 / p)
    v_pqw = np.array([-vmag * np.sin(nu_rad),
                      vmag * (e + np.cos(nu_rad)),
                      0.0])
    cO, sO = np.cos(raan_rad), np.sin(raan_rad)
    ci, si = np.cos(i_rad), np.sin(i_rad)
    cw, sw = np.cos(argp_rad), np.sin(argp_rad)
    R = np.array([
        [cO * cw - sO * sw * ci, -cO * sw - sO * cw * ci,  sO * si],
        [sO * cw + cO * sw * ci, -sO * sw + cO * cw * ci, -cO * si],
        [sw * si,                 cw * si,                 ci],
    ])
    return R @ r_pqw, R @ v_pqw


def raan_for_apogee_over_station(i_rad: float, argp_rad: float,
                                 target_lon_deg: float, jd: float) -> float:
    """Solve for RAAN so the apogee sub-satellite ECEF longitude at jd
    equals target_lon_deg."""
    gmst_rad = ve_hpop.gmst_rad(jd)

    def apogee_lon_ecef(Omega: float) -> float:
        r_eci, _ = keplerian_to_eci(1.0, 0.0, i_rad, Omega, argp_rad, np.pi)
        lon_eci = np.arctan2(r_eci[1], r_eci[0])
        d = lon_eci - gmst_rad - np.deg2rad(target_lon_deg)
        return np.arctan2(np.sin(d), np.cos(d))

    scan = np.linspace(0.0, 2.0 * np.pi, 361)
    residuals = np.array([apogee_lon_ecef(o) for o in scan])
    for k in range(len(scan) - 1):
        if residuals[k] * residuals[k + 1] < 0.0:
            return brentq(apogee_lon_ecef, scan[k], scan[k + 1])
    raise RuntimeError("RAAN root not found (apogee-over-station geometry)")


# ==========================================================================
# Dynamics
# ==========================================================================
def two_body_j2_rhs(_t: float, state: np.ndarray) -> np.ndarray:
    """r'' = -mu r/|r|^3 + J2 acceleration in ECI (Vallado eq 8-32)."""
    r = state[0:3]
    v = state[3:6]
    r_norm2 = float(r @ r)
    r_norm = np.sqrt(r_norm2)
    a_two_body = -MU_KM3_S2 * r / (r_norm2 * r_norm)
    z_over_r = r[2] / r_norm
    factor = 1.5 * J2 * MU_KM3_S2 * R_EARTH_KM * R_EARTH_KM / (r_norm2 * r_norm2 * r_norm)
    coef_xy = 5.0 * z_over_r * z_over_r - 1.0
    coef_z = 5.0 * z_over_r * z_over_r - 3.0
    a_j2 = factor * np.array([r[0] * coef_xy, r[1] * coef_xy, r[2] * coef_z])
    return np.concatenate([v, a_two_body + a_j2])


def integrate_dynamics(state6: np.ndarray, t0: float, t1: float,
                       rtol: float = 1e-10, atol: float = 1e-11) -> np.ndarray:
    """Integrate two-body+J2 from t0 to t1 (seconds), return final state."""
    if t1 == t0:
        return state6.copy()
    sol = solve_ivp(two_body_j2_rhs, (t0, t1), state6,
                    method='DOP853', rtol=rtol, atol=atol,
                    max_step=300.0)
    if not sol.success:
        raise RuntimeError(f"solve_ivp failed: {sol.message}")
    return sol.y[:, -1]


def dynamics_stm_two_body_j2(state6: np.ndarray, dt: float) -> np.ndarray:
    """First-order state-transition matrix for the 6-state r,v dynamics.

    Uses the two-body linearisation of da/dr = -mu/r^3 (I - 3 r_hat r_hat^T);
    J2 gradient contribution is small at Molniya altitudes and folded into Q.
    For short dt (<= a few minutes) this is more than adequate at Molniya's
    slow apogee dynamics; over the shorter perigee arc the residual bias is
    absorbed by process noise.
    """
    r = state6[0:3]
    r_norm = np.linalg.norm(r)
    r_hat = r / r_norm
    G = -(MU_KM3_S2 / r_norm ** 3) * (np.eye(3) - 3.0 * np.outer(r_hat, r_hat))
    F = np.eye(6)
    F[0:3, 3:6] = np.eye(3) * dt
    F[3:6, 0:3] = G * dt
    F[0:3, 0:3] += 0.5 * G * dt * dt
    return F


# ==========================================================================
# Station kinematics + measurement model
# (Structurally identical to src/od/doppler_measurement.cpp.)
# ==========================================================================
def station_ecef_km(lat_deg: float, lon_deg: float, alt_km: float) -> np.ndarray:
    lat = np.deg2rad(lat_deg)
    lon = np.deg2rad(lon_deg)
    sinL, cosL = np.sin(lat), np.cos(lat)
    N = WGS84_A_KM / np.sqrt(1.0 - WGS84_E2 * sinL * sinL)
    x = (N + alt_km) * cosL * np.cos(lon)
    y = (N + alt_km) * cosL * np.sin(lon)
    z = (N * (1.0 - WGS84_E2) + alt_km) * sinL
    return np.array([x, y, z])


def station_state_teme(lat_deg: float, lon_deg: float, alt_km: float,
                       jd: float):
    """Rotate WGS-84 station ECEF to TEME via GMST. Returns (r_km, v_km_s)."""
    p_ecef = station_ecef_km(lat_deg, lon_deg, alt_km)
    theta = ve_hpop.gmst_rad(jd)
    c, s = np.cos(theta), np.sin(theta)
    r = np.array([c * p_ecef[0] - s * p_ecef[1],
                  s * p_ecef[0] + c * p_ecef[1],
                  p_ecef[2]])
    v = np.array([-OMEGA_EARTH_RAD_S * r[1],
                   OMEGA_EARTH_RAD_S * r[0],
                   0.0])
    return r, v


def elevation_deg(r_sat: np.ndarray, r_stn: np.ndarray) -> float:
    los = r_sat - r_stn
    los_hat = los / np.linalg.norm(los)
    stn_hat = r_stn / np.linalg.norm(r_stn)
    return float(np.rad2deg(np.arcsin(np.clip(los_hat @ stn_hat, -1.0, 1.0))))


def predict_doppler(state8: np.ndarray, jd: float, t_since_ref: float,
                    station_geo: tuple):
    """SR Doppler + oscillator bias/drift with first-order light-time
    correction (same construction as src/od/doppler_measurement.cpp)."""
    r_sat = state8[0:3]
    v_sat = state8[3:6]
    bc = state8[6]
    bd = state8[7]
    r_stn, v_stn = station_state_teme(*station_geo, jd)
    rho_inst = r_sat - r_stn
    tau_sec = np.linalg.norm(rho_inst) / C_KM_S
    r_sat_ret = r_sat - v_sat * tau_sec
    rho = r_sat_ret - r_stn
    s = np.linalg.norm(rho)
    rho_hat = rho / s
    v_rel = v_sat - v_stn
    beta_los = float(v_rel @ rho_hat) / C_KM_S
    beta_sq = float(v_rel @ v_rel) / (C_KM_S * C_KM_S)
    f_sr = F_TRANSMIT_HZ * np.sqrt(max(0.0, 1.0 - beta_sq)) / (1.0 + beta_los)
    return f_sr + bc + bd * t_since_ref, s, beta_los


def doppler_jacobian(state8: np.ndarray, jd: float, t_since_ref: float,
                     station_geo: tuple) -> np.ndarray:
    """dh/dx at the instantaneous limit (O(v/c) discrepancy from the light-
    time-corrected h(x); absorbed by Q for LEO/Molniya). Same formula as
    ve::od::doppler_jacobian."""
    r_sat = state8[0:3]
    v_sat = state8[3:6]
    r_stn, v_stn = station_state_teme(*station_geo, jd)
    rho = r_sat - r_stn
    s = np.linalg.norm(rho)
    rho_hat = rho / s
    v_rel = v_sat - v_stn
    c = C_KM_S
    g = float(v_rel @ rho_hat) / c
    b2 = float(v_rel @ v_rel) / (c * c)
    sqrt_1m_b2 = np.sqrt(max(0.0, 1.0 - b2))
    one_plus_g = 1.0 + g
    A = sqrt_1m_b2 / one_plus_g
    K_g = -F_TRANSMIT_HZ * A / one_plus_g
    dg_dr = (v_rel - rho_hat * g * c) / (c * s)
    vel_pref_b2 = -F_TRANSMIT_HZ / (c * c * max(sqrt_1m_b2, 1e-15)) / one_plus_g
    J = np.zeros(8)
    J[0:3] = K_g * dg_dr
    J[3:6] = K_g * rho_hat / c + vel_pref_b2 * v_rel
    J[6] = 1.0
    J[7] = t_since_ref
    return J


# ==========================================================================
# EKF
# ==========================================================================
def process_noise_Q(dt: float) -> np.ndarray:
    """Van Loan CV block + white oscillator (matches ve::od::orbit_ssm)."""
    Q = np.zeros((8, 8))
    dt = abs(dt)
    if dt == 0.0:
        return Q
    q_a2 = Q_ACCEL_KM_S2_SQRT_S ** 2
    q_bc2 = Q_BC_HZ_SQRT_S ** 2
    q_bd2 = Q_BD_HZ_S_SQRT_S ** 2
    dt2 = dt * dt
    dt3 = dt2 * dt
    for i in range(3):
        Q[i, i] = q_a2 * dt3 / 3.0
        Q[3 + i, 3 + i] = q_a2 * dt
        Q[i, 3 + i] = q_a2 * dt2 / 2.0
        Q[3 + i, i] = q_a2 * dt2 / 2.0
    Q[6, 6] = q_bc2 * dt
    Q[7, 7] = q_bd2 * dt
    return Q


MAX_STM_STEP_SEC = 300.0


def ekf_predict(x: np.ndarray, P: np.ndarray, dt: float):
    """Nonlinear state propagation + covariance propagation with STM chaining.

    First-order dynamics_stm_two_body_j2 is only accurate over short dt; when
    the satellite is below the horizon for many minutes we subdivide the
    interval into <= MAX_STM_STEP_SEC chunks so the state-transition matrix
    accumulates as a product of well-conditioned sub-STMs.
    """
    if dt == 0.0:
        return x.copy(), P.copy()
    n_sub = max(1, int(np.ceil(abs(dt) / MAX_STM_STEP_SEC)))
    h = dt / n_sub
    x_cur = x.copy()
    P_cur = P.copy()
    for _ in range(n_sub):
        F = np.eye(8)
        F[0:6, 0:6] = dynamics_stm_two_body_j2(x_cur[0:6], h)
        F[6, 7] = h
        x_next = np.empty(8)
        x_next[0:6] = integrate_dynamics(x_cur[0:6], 0.0, h)
        x_next[6] = x_cur[6] + x_cur[7] * h
        x_next[7] = x_cur[7]
        P_cur = F @ P_cur @ F.T + process_noise_Q(h)
        P_cur = 0.5 * (P_cur + P_cur.T)
        x_cur = x_next
    return x_cur, P_cur


def ekf_update(x_pred: np.ndarray, P_pred: np.ndarray, y_hz: float,
               jd: float, t_since_ref: float, station_geo: tuple):
    """Scalar Doppler update."""
    h, _, _ = predict_doppler(x_pred, jd, t_since_ref, station_geo)
    innov = y_hz - h
    H = doppler_jacobian(x_pred, jd, t_since_ref, station_geo)
    R = SIGMA_R_HZ * SIGMA_R_HZ
    HP = H @ P_pred
    S = float(HP @ H) + R
    K = (P_pred @ H) / S
    x_upd = x_pred + K * innov
    # Joseph form for numerical stability.
    I_KH = np.eye(8) - np.outer(K, H)
    P_upd = I_KH @ P_pred @ I_KH.T + (R * np.outer(K, K))
    P_upd = 0.5 * (P_upd + P_upd.T)
    return x_upd, P_upd, innov, innov * innov / S


# ==========================================================================
# Main simulation
# ==========================================================================
def main():
    print("MolniyaTwelveHour: 3-day Doppler OD demonstration")
    print("==================================================")

    # ---- 1. Set up truth trajectory --------------------------------------
    jd_epoch = utc_to_jd(EPOCH_UTC)
    i_rad = np.deg2rad(INCLINATION_DEG)
    argp_rad = np.deg2rad(ARG_PERIGEE_DEG)
    Omega_rad = raan_for_apogee_over_station(
        i_rad, argp_rad, STATION_LON_DEG, jd_epoch)
    nu_rad = np.deg2rad(TRUE_ANOM_AT_EPOCH_DEG)
    r0_true, v0_true = keplerian_to_eci(
        SEMI_MAJOR_KM, ECCENTRICITY, i_rad, Omega_rad, argp_rad, nu_rad)

    print(f"Epoch:      {EPOCH_UTC.isoformat()}   JD {jd_epoch:.6f}")
    print(f"Station:    Gaithersburg, MD  ({STATION_LAT_DEG:+.4f} N, "
          f"{STATION_LON_DEG:+.4f} E, {STATION_ALT_KM*1000:.0f} m)")
    print(f"Elements:   a={SEMI_MAJOR_KM:.1f} km  e={ECCENTRICITY}  "
          f"i={INCLINATION_DEG} deg  argp={ARG_PERIGEE_DEG} deg  "
          f"RAAN={np.rad2deg(Omega_rad):.3f} deg")
    print(f"Apogee r0 = {np.linalg.norm(r0_true):.3f} km    "
          f"|v0| = {np.linalg.norm(v0_true):.3f} km/s")

    # ---- 2. Truth propagation and observation generation ----------------
    duration_sec = DURATION_DAYS * 86400.0
    print(f"\nGenerating truth trajectory + noisy Doppler observations "
          f"({DURATION_DAYS} days at {OBS_CADENCE_SEC:.0f} s cadence)...")
    rng = np.random.default_rng(20260805)
    times_sec = np.arange(0.0, duration_sec + 1e-9, OBS_CADENCE_SEC)

    truth_state = np.concatenate([r0_true, v0_true])
    truth_by_t = {0.0: truth_state.copy()}
    observations = []
    peak_elev_seen = -90.0
    t_prev = 0.0
    for t in times_sec:
        if t > t_prev:
            truth_state = integrate_dynamics(truth_state, 0.0, t - t_prev)
        truth_by_t[t] = truth_state.copy()
        t_prev = t
        jd = jd_epoch + t / 86400.0
        r_stn, _ = station_state_teme(
            STATION_LAT_DEG, STATION_LON_DEG, STATION_ALT_KM, jd)
        el = elevation_deg(truth_state[0:3], r_stn)
        if el > peak_elev_seen:
            peak_elev_seen = el
        if el < MIN_ELEV_DEG:
            continue
        state_with_bias = np.concatenate([
            truth_state[0:6],
            [TRUTH_BC_HZ, TRUTH_BD_HZ_S],
        ])
        f_true, _, _ = predict_doppler(
            state_with_bias, jd, t,
            (STATION_LAT_DEG, STATION_LON_DEG, STATION_ALT_KM))
        f_meas = f_true + rng.normal(0.0, SIGMA_R_HZ)
        observations.append(dict(
            t_sec=t, jd=jd, f_hz=f_meas, elev_deg=el))

    print(f"Peak elevation from station across 3 days: {peak_elev_seen:.2f} deg")
    print(f"Observations (elev >= {MIN_ELEV_DEG:.0f} deg): "
          f"{len(observations)} of {len(times_sec)} time samples")

    # ---- 3. Filter prior with TLE-error-representative perturbation -----
    v_hat = v0_true / np.linalg.norm(v0_true)
    r0_prior = r0_true + PRIOR_ALONGTRACK_POS_KM * v_hat
    v0_prior = v0_true + PRIOR_ALONGTRACK_VEL_KM_S * v_hat
    x0 = np.concatenate([r0_prior, v0_prior, [PRIOR_BC_HZ, PRIOR_BD_HZ_S]])
    # Diagonal P0 tightly matched to the perturbation magnitude. A looser
    # P0 lets S = HPH^T + R be dominated by prior projections through H,
    # which drives NIS very low and slows the filter's uptake of information
    # from each observation.
    P0 = np.diag([
        PRIOR_ALONGTRACK_POS_KM ** 2,
        PRIOR_ALONGTRACK_POS_KM ** 2,
        PRIOR_ALONGTRACK_POS_KM ** 2,
        PRIOR_ALONGTRACK_VEL_KM_S ** 2,
        PRIOR_ALONGTRACK_VEL_KM_S ** 2,
        PRIOR_ALONGTRACK_VEL_KM_S ** 2,
        (30.0) ** 2,
        (0.002) ** 2,
    ])
    prior_pos_err_km = np.linalg.norm(r0_prior - r0_true)
    prior_vel_err_m_s = np.linalg.norm(v0_prior - v0_true) * 1000.0
    print(f"\nInitial prior error: {prior_pos_err_km:.3f} km position, "
          f"{prior_vel_err_m_s:.3f} m/s velocity, "
          f"{PRIOR_BC_HZ - TRUTH_BC_HZ:+.1f} Hz oscillator bias")

    # ---- 4. Run EKF over the observations -------------------------------
    print(f"\nRunning EKF over {len(observations)} Doppler observations...")
    x = x0.copy()
    P = P0.copy()
    t_prev = 0.0
    pos_errors_km = []
    vel_errors_m_s = []
    nis_history = []
    innov_history = []
    epoch_secs = []
    for obs in observations:
        t_now = obs['t_sec']
        x, P = ekf_predict(x, P, t_now - t_prev)
        x, P, innov, nis = ekf_update(
            x, P, obs['f_hz'], obs['jd'], obs['t_sec'],
            (STATION_LAT_DEG, STATION_LON_DEG, STATION_ALT_KM))
        t_prev = t_now
        # Compare against truth (already stored at this observation time)
        truth = truth_by_t[t_now]
        pos_errors_km.append(np.linalg.norm(x[0:3] - truth[0:3]))
        vel_errors_m_s.append(np.linalg.norm(x[3:6] - truth[3:6]) * 1000.0)
        nis_history.append(nis)
        innov_history.append(innov)
        epoch_secs.append(t_now)

    # ---- 5. Report ------------------------------------------------------
    pos = np.array(pos_errors_km)
    vel = np.array(vel_errors_m_s)
    nis_arr = np.array(nis_history)
    innov_arr = np.array(innov_history)
    tsec = np.array(epoch_secs)
    tdays = tsec / 86400.0

    print("\n=== Convergence summary ===")
    print(f"n observations:     {len(pos)}")
    n_head = min(20, len(pos))
    n_tail = max(n_head, len(pos) // 8)
    print(f"First {n_head} obs   pos err = {pos[:n_head].mean():8.3f} km   "
          f"vel err = {vel[:n_head].mean():8.3f} m/s")
    print(f"Last  {n_tail} obs  pos err = {pos[-n_tail:].mean():8.3f} km   "
          f"vel err = {vel[-n_tail:].mean():8.3f} m/s")
    print(f"Median pos err (post-first-pass): "
          f"{np.median(pos[len(pos)//4:]):.3f} km")
    print(f"Median vel err (post-first-pass): "
          f"{np.median(vel[len(vel)//4:]):.3f} m/s")

    # Innovation statistics: healthy filter has |innov/sigma_R| ~ 1
    innov_std_hz = float(np.std(innov_arr[len(innov_arr) // 4:]))
    print(f"\nInnovation std (post-first-pass): {innov_std_hz:.2f} Hz "
          f"(sigma_R = {SIGMA_R_HZ:.1f} Hz)")
    print(f"Mean NIS (post-first-pass): "
          f"{np.mean(nis_arr[len(nis_arr) // 4:]):.3f} "
          f"(target ~1.0 for NY=1)")

    bc_final = x[6]
    bd_final = x[7]
    truth_bc_at_end = TRUTH_BC_HZ + TRUTH_BD_HZ_S * tsec[-1]
    print(f"\nOscillator recovery at t = {tdays[-1]:.2f} d:")
    print(f"  bc  filter = {bc_final:8.3f} Hz     truth = {truth_bc_at_end:8.3f} Hz "
          f"({bc_final - truth_bc_at_end:+.3f} Hz)")
    print(f"  bd  filter = {bd_final*1e3:8.3f} mHz/s  truth = {TRUTH_BD_HZ_S*1e3:8.3f} mHz/s "
          f"({(bd_final - TRUTH_BD_HZ_S)*1e3:+.3f} mHz/s)")

    # Per-day timeline (median position error binned by 24-hour window)
    print("\nPer-day position error (median km):")
    for d in range(int(np.ceil(DURATION_DAYS))):
        lo, hi = d * 86400.0, (d + 1) * 86400.0
        mask = (tsec >= lo) & (tsec < hi)
        if mask.sum() > 0:
            print(f"  Day {d + 1}  n={mask.sum():4d}  "
                  f"median = {np.median(pos[mask]):.3f} km,  "
                  f"90th pctile = {np.quantile(pos[mask], 0.9):.3f} km")


if __name__ == "__main__":
    main()
