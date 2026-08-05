"""Live orbit-determination EKF backend for the Qt dashboard.

Mirrors the eight-state layout, Van Loan process-noise Q, and analytical
Doppler Jacobian used by the audited C++ ``ve::od`` subsystem, so the
online estimate is consistent with what a batch smoother would produce
on the same observation stream.

This module is used by ``qt_dashboards/live_od_panel.py``. It is standalone
(``import live_od_filter``) and depends only on numpy, scipy, and the
existing ``ve_hpop`` pybind11 module for physical constants + GMST. If
``ve_od`` (the OD pybind11 module) is available its ``teme_state_at_epoch``
helper is used for TLE seeding; otherwise the module falls back to sgp4.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Optional, Sequence

import numpy as np
from scipy.integrate import solve_ivp


# ---------------------------------------------------------------------------
# ve_hpop for constants + GMST (single source of truth with the C++ side).
# ---------------------------------------------------------------------------
def _import_ve_hpop():
    import os, sys
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (os.path.join(os.path.dirname(here), "build"),
                      os.path.join(os.path.dirname(here), "build_od")):
        if candidate not in sys.path:
            sys.path.insert(0, candidate)
    import ve_hpop
    return ve_hpop


_VE_HPOP = _import_ve_hpop()

MU_KM3_S2 = _VE_HPOP.GM_KM3_S2
R_EARTH_KM = _VE_HPOP.EARTH_RADIUS_KM
J2 = 1.0826262e-3
OMEGA_EARTH_RAD_S = 7.2921159e-5
C_KM_S = 299792.458

WGS84_A_KM = 6378.137
WGS84_F = 1.0 / 298.257223563
WGS84_E2 = 2.0 * WGS84_F - WGS84_F * WGS84_F


# ---------------------------------------------------------------------------
# TLE handling. Prefer ve_od (audited); fall back to sgp4 if unavailable.
# ---------------------------------------------------------------------------
def _import_ve_od():
    import os, sys
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (os.path.join(os.path.dirname(here), "build"),
                      os.path.join(os.path.dirname(here), "build_od")):
        if candidate not in sys.path:
            sys.path.insert(0, candidate)
    try:
        import ve_od
        return ve_od
    except ImportError:
        return None


def tle_epoch_state(name: str, line1: str, line2: str) -> tuple[datetime, float, np.ndarray, np.ndarray]:
    """Return (epoch_utc, jd_epoch, r_km, v_km_s) for the TLE at its epoch."""
    ve_od = _import_ve_od()
    if ve_od is not None:
        s = ve_od.teme_state_at_epoch(name, line1, line2)
        r = np.array([s.r_km.x, s.r_km.y, s.r_km.z])
        v = np.array([s.v_km_s.x, s.v_km_s.y, s.v_km_s.z])
        return s.t_epoch_utc, s.jd_epoch, r, v
    # Fallback: sgp4 Python package
    from sgp4.api import Satrec, jday
    sat = Satrec.twoline2rv(line1, line2)
    jd = sat.jdsatepoch + sat.jdsatepochF
    e, r, v = sat.sgp4(sat.jdsatepoch, sat.jdsatepochF)
    if e:
        raise RuntimeError(f"sgp4 error {e} for {name}")
    unix = (jd - 2440587.5) * 86400.0
    epoch = datetime.fromtimestamp(unix, tz=timezone.utc)
    return epoch, jd, np.array(r), np.array(v)


# ---------------------------------------------------------------------------
# Dynamics: two-body + J2 (matches MolniyaTwelveHour.py; enough for LEO/MEO
# tracking at metre-scale sigma_R noise floors on a per-pass timescale).
# ---------------------------------------------------------------------------
def two_body_j2_rhs(_t: float, s: np.ndarray) -> np.ndarray:
    r = s[0:3]; v = s[3:6]
    r2 = float(r @ r); rn = np.sqrt(r2)
    a_kep = -MU_KM3_S2 * r / (r2 * rn)
    zr = r[2] / rn
    f = 1.5 * J2 * MU_KM3_S2 * R_EARTH_KM * R_EARTH_KM / (r2 * r2 * rn)
    kxy = 5.0 * zr * zr - 1.0
    kz = 5.0 * zr * zr - 3.0
    a_j2 = f * np.array([r[0] * kxy, r[1] * kxy, r[2] * kz])
    return np.concatenate([v, a_kep + a_j2])


def integrate_two_body_j2(s0: np.ndarray, dt: float, rtol=1e-9, atol=1e-10) -> np.ndarray:
    if dt == 0.0:
        return s0.copy()
    sol = solve_ivp(two_body_j2_rhs, (0.0, dt), s0, method='DOP853',
                    rtol=rtol, atol=atol, max_step=300.0)
    if not sol.success:
        raise RuntimeError(f"solve_ivp failed: {sol.message}")
    return sol.y[:, -1]


def stm_two_body(state6: np.ndarray, dt: float) -> np.ndarray:
    r = state6[0:3]; rn = float(np.linalg.norm(r)); rh = r / rn
    G = -(MU_KM3_S2 / rn ** 3) * (np.eye(3) - 3.0 * np.outer(rh, rh))
    F = np.eye(6)
    F[0:3, 3:6] = np.eye(3) * dt
    F[3:6, 0:3] = G * dt
    F[0:3, 0:3] += 0.5 * G * dt * dt
    return F


# ---------------------------------------------------------------------------
# Station kinematics + Doppler measurement (mirrors ve::od).
# ---------------------------------------------------------------------------
def utc_to_jd(t: datetime) -> float:
    unix = t.replace(tzinfo=timezone.utc if t.tzinfo is None else t.tzinfo).timestamp()
    return 2440587.5 + unix / 86400.0


def station_ecef_km(lat_deg, lon_deg, alt_km):
    lat = np.deg2rad(lat_deg); lon = np.deg2rad(lon_deg)
    sl, cl = np.sin(lat), np.cos(lat)
    N = WGS84_A_KM / np.sqrt(1.0 - WGS84_E2 * sl * sl)
    return np.array([(N + alt_km) * cl * np.cos(lon),
                     (N + alt_km) * cl * np.sin(lon),
                     (N * (1.0 - WGS84_E2) + alt_km) * sl])


def station_state_teme(lat_deg, lon_deg, alt_km, jd):
    p = station_ecef_km(lat_deg, lon_deg, alt_km)
    theta = _VE_HPOP.gmst_rad(jd)
    c, s = np.cos(theta), np.sin(theta)
    r = np.array([c * p[0] - s * p[1], s * p[0] + c * p[1], p[2]])
    v = np.array([-OMEGA_EARTH_RAD_S * r[1], OMEGA_EARTH_RAD_S * r[0], 0.0])
    return r, v


def elevation_deg(r_sat, r_stn):
    los = r_sat - r_stn
    los_hat = los / np.linalg.norm(los)
    stn_hat = r_stn / np.linalg.norm(r_stn)
    return float(np.rad2deg(np.arcsin(np.clip(los_hat @ stn_hat, -1.0, 1.0))))


def predict_doppler(state8, jd, t_since_ref, station_geo, f_transmit_hz):
    r_sat = state8[0:3]; v_sat = state8[3:6]
    bc = state8[6]; bd = state8[7]
    r_stn, v_stn = station_state_teme(*station_geo, jd)
    rho_i = r_sat - r_stn
    tau = np.linalg.norm(rho_i) / C_KM_S
    r_sat_ret = r_sat - v_sat * tau
    rho = r_sat_ret - r_stn
    s = np.linalg.norm(rho)
    rho_hat = rho / s
    v_rel = v_sat - v_stn
    beta_los = float(v_rel @ rho_hat) / C_KM_S
    beta_sq = float(v_rel @ v_rel) / (C_KM_S * C_KM_S)
    f_sr = f_transmit_hz * np.sqrt(max(0.0, 1.0 - beta_sq)) / (1.0 + beta_los)
    return f_sr + bc + bd * t_since_ref, s, beta_los


def doppler_jacobian(state8, jd, t_since_ref, station_geo, f_transmit_hz):
    r_sat = state8[0:3]; v_sat = state8[3:6]
    r_stn, v_stn = station_state_teme(*station_geo, jd)
    rho = r_sat - r_stn
    s = np.linalg.norm(rho)
    rho_hat = rho / s
    v_rel = v_sat - v_stn
    c = C_KM_S
    g = float(v_rel @ rho_hat) / c
    b2 = float(v_rel @ v_rel) / (c * c)
    sq = np.sqrt(max(0.0, 1.0 - b2))
    opg = 1.0 + g
    K_g = -f_transmit_hz * sq / (opg * opg)
    dg_dr = (v_rel - rho_hat * g * c) / (c * s)
    vpre = -f_transmit_hz / (c * c * max(sq, 1e-15)) / opg
    J = np.zeros(8)
    J[0:3] = K_g * dg_dr
    J[3:6] = K_g * rho_hat / c + vpre * v_rel
    J[6] = 1.0
    J[7] = t_since_ref
    return J


# ---------------------------------------------------------------------------
# EKF configuration and driver
# ---------------------------------------------------------------------------
@dataclass
class LiveOdConfig:
    """Live-OD scenario configuration."""
    station_lat_deg: float
    station_lon_deg: float
    station_alt_km: float = 0.0
    f_transmit_hz: float = 2.4e9
    sigma_R_hz: float = 5.0
    q_accel_km_s2_sqrt_s: float = 1.0e-8
    q_bc_hz_sqrt_s: float = 0.05
    q_bd_hz_s_sqrt_s: float = 0.005
    prior_pos_km: float = 1.0          # 1-sigma per position axis
    prior_vel_km_s: float = 0.001      # 1-sigma per velocity axis (1 m/s)
    prior_bc_hz: float = 50.0
    prior_bd_hz_s: float = 0.005
    max_stm_step_sec: float = 300.0


@dataclass
class LiveOdSnapshot:
    """A single filter epoch, emitted after each measurement update."""
    t_utc: datetime
    r_km: np.ndarray
    v_km_s: np.ndarray
    bc_hz: float
    bd_hz_s: float
    sigma_pos_km: float          # sqrt(trace(P_rr)/3)
    sigma_vel_km_s: float
    innov_hz: float
    nis: float
    n_updates: int


class LiveOdFilter:
    """Streaming 8-state EKF driven by scalar Doppler observations."""

    def __init__(self, cfg: LiveOdConfig, t_ref_utc: datetime,
                 jd_ref: float, x0: np.ndarray):
        self.cfg = cfg
        self.t_ref = t_ref_utc
        self.jd_ref = jd_ref
        self.x = np.asarray(x0, dtype=float).copy()
        self.P = np.diag([
            cfg.prior_pos_km ** 2, cfg.prior_pos_km ** 2, cfg.prior_pos_km ** 2,
            cfg.prior_vel_km_s ** 2, cfg.prior_vel_km_s ** 2, cfg.prior_vel_km_s ** 2,
            cfg.prior_bc_hz ** 2, cfg.prior_bd_hz_s ** 2,
        ])
        self.t_prev_sec = 0.0
        self.n_updates = 0

    def _q_matrix(self, dt: float) -> np.ndarray:
        dt = abs(dt)
        if dt == 0.0:
            return np.zeros((8, 8))
        Q = np.zeros((8, 8))
        qa2 = self.cfg.q_accel_km_s2_sqrt_s ** 2
        qbc2 = self.cfg.q_bc_hz_sqrt_s ** 2
        qbd2 = self.cfg.q_bd_hz_s_sqrt_s ** 2
        dt2 = dt * dt; dt3 = dt2 * dt
        for i in range(3):
            Q[i, i] = qa2 * dt3 / 3.0
            Q[3 + i, 3 + i] = qa2 * dt
            Q[i, 3 + i] = qa2 * dt2 / 2.0
            Q[3 + i, i] = qa2 * dt2 / 2.0
        Q[6, 6] = qbc2 * dt
        Q[7, 7] = qbd2 * dt
        return Q

    def _predict(self, dt: float):
        if dt == 0.0:
            return
        n_sub = max(1, int(np.ceil(abs(dt) / self.cfg.max_stm_step_sec)))
        h = dt / n_sub
        for _ in range(n_sub):
            F = np.eye(8)
            F[0:6, 0:6] = stm_two_body(self.x[0:6], h)
            F[6, 7] = h
            x_next = np.empty(8)
            x_next[0:6] = integrate_two_body_j2(self.x[0:6], h)
            x_next[6] = self.x[6] + self.x[7] * h
            x_next[7] = self.x[7]
            self.P = F @ self.P @ F.T + self._q_matrix(h)
            self.P = 0.5 * (self.P + self.P.T)
            self.x = x_next

    def update(self, t_utc: datetime, f_meas_hz: float) -> LiveOdSnapshot:
        """Ingest one Doppler observation and return the filter snapshot."""
        t_sec = (t_utc - self.t_ref).total_seconds()
        dt = t_sec - self.t_prev_sec
        self._predict(dt)
        jd = self.jd_ref + t_sec / 86400.0
        station = (self.cfg.station_lat_deg, self.cfg.station_lon_deg,
                   self.cfg.station_alt_km)
        h_pred, _, _ = predict_doppler(self.x, jd, t_sec, station,
                                       self.cfg.f_transmit_hz)
        H = doppler_jacobian(self.x, jd, t_sec, station,
                             self.cfg.f_transmit_hz)
        innov = f_meas_hz - h_pred
        R = self.cfg.sigma_R_hz ** 2
        S = float(H @ self.P @ H) + R
        K = (self.P @ H) / S
        self.x = self.x + K * innov
        I_KH = np.eye(8) - np.outer(K, H)
        self.P = I_KH @ self.P @ I_KH.T + R * np.outer(K, K)
        self.P = 0.5 * (self.P + self.P.T)
        self.t_prev_sec = t_sec
        self.n_updates += 1
        return LiveOdSnapshot(
            t_utc=t_utc,
            r_km=self.x[0:3].copy(),
            v_km_s=self.x[3:6].copy(),
            bc_hz=float(self.x[6]),
            bd_hz_s=float(self.x[7]),
            sigma_pos_km=float(np.sqrt(np.trace(self.P[0:3, 0:3]) / 3.0)),
            sigma_vel_km_s=float(np.sqrt(np.trace(self.P[3:6, 3:6]) / 3.0)),
            innov_hz=float(innov),
            nis=float(innov * innov / S),
            n_updates=self.n_updates,
        )
