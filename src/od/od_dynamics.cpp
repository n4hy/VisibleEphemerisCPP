// od_dynamics.cpp -- Fixed-step RK4 sigma-point orbital integrator.
//
// The ForceModel is called with the Julian Date corresponding to each
// intermediate stage (k1, k2, k3, k4) so that time-varying forces (Sun/Moon
// third-body, atmospheric density via geometric altitude in Earth-fixed
// frame) see the right instant. jd = jd_at_t_ref + (t_sec - t_ref_sec)/86400.

#include "od/od_dynamics.hpp"

#include <cmath>

namespace ve::od {

namespace {

// One RK4 step of dt seconds, starting from (r, v) at time t_sec.
void rk4_step(const ve::ForceModel& fm,
              double jd_at_t_ref, double t_ref_sec,
              double t_sec, double dt,
              const ve::Vector3& r, const ve::Vector3& v,
              ve::Vector3& r_out, ve::Vector3& v_out)
{
    // Convert a time-in-seconds offset into a Julian Date.
    auto jd_at = [&](double ts) {
        return jd_at_t_ref + (ts - t_ref_sec) / ve::SECONDS_PER_DAY;
    };

    // k1
    const ve::Vector3 k1v = v;
    const ve::Vector3 k1a = fm.acceleration(jd_at(t_sec), r, v);

    // k2
    const ve::Vector3 r2 = r + k1v * (dt * 0.5);
    const ve::Vector3 v2 = v + k1a * (dt * 0.5);
    const ve::Vector3 k2v = v2;
    const ve::Vector3 k2a = fm.acceleration(jd_at(t_sec + 0.5 * dt), r2, v2);

    // k3
    const ve::Vector3 r3 = r + k2v * (dt * 0.5);
    const ve::Vector3 v3 = v + k2a * (dt * 0.5);
    const ve::Vector3 k3v = v3;
    const ve::Vector3 k3a = fm.acceleration(jd_at(t_sec + 0.5 * dt), r3, v3);

    // k4
    const ve::Vector3 r4 = r + k3v * dt;
    const ve::Vector3 v4 = v + k3a * dt;
    const ve::Vector3 k4v = v4;
    const ve::Vector3 k4a = fm.acceleration(jd_at(t_sec + dt), r4, v4);

    r_out = r + (k1v + 2.0 * k2v + 2.0 * k3v + k4v) * (dt / 6.0);
    v_out = v + (k1a + 2.0 * k2a + 2.0 * k3a + k4a) * (dt / 6.0);
}

} // namespace

std::pair<ve::Vector3, ve::Vector3> integrate_rk4(
    const ve::ForceModel& fm,
    double jd_at_t_ref, double t_ref_sec,
    const ve::Vector3& r_start, const ve::Vector3& v_start,
    double t_end_sec, double max_step_sec)
{
    ve::Vector3 r = r_start;
    ve::Vector3 v = v_start;
    double t = t_ref_sec;
    const double t_end = t_end_sec;
    const double total = t_end - t;
    if (total == 0.0) return {r, v};

    const double step_sign = (total > 0.0) ? 1.0 : -1.0;
    const double abs_max = std::abs(max_step_sec) > 0.0
                           ? std::abs(max_step_sec) : 0.5;

    while ((step_sign > 0 && t < t_end) || (step_sign < 0 && t > t_end)) {
        const double remaining = t_end - t;
        const double dt_mag = std::min(abs_max, std::abs(remaining));
        const double dt = step_sign * dt_mag;
        ve::Vector3 r_next, v_next;
        rk4_step(fm, jd_at_t_ref, t_ref_sec, t, dt, r, v, r_next, v_next);
        r = r_next;
        v = v_next;
        t += dt;
    }
    return {r, v};
}

} // namespace ve::od
