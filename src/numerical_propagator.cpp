// numerical_propagator.cpp - Adaptive Fehlberg RK7(8) integrator + propagator.
//
// Owns a ForceModel and integrates the equations of motion d/dt[r,v] = [v, a]
// from the TLE-epoch state vector (obtained by evaluating SGP4 at epoch) to any
// requested time. The integrator is an 8th-order embedded Runge-Kutta-Fehlberg
// method with 7th-order error estimation and adaptive step-size control; the
// Butcher tableau is the verified NASA TR R-287 set (see the anonymous-namespace
// block below). A marching checkpoint is cached so sequential queries advance
// cheaply; the nearest of {epoch anchor, last result} is used as each start.
// All public entry points are mutex-guarded for thread-safe per-satellite use.
#include "numerical_propagator.hpp"
#include "logger.hpp"
#include <SGP4.h>
#include <Eci.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace ve {
    namespace {
        // Fehlberg RK7(8) Butcher tableau (NASA TR R-287; coefficients verified
        // against NASA Trick er7_utils rkf78_butcher_tableau, every row sum == c_i).
        constexpr int NS = 13;
        const double A[NS][NS] = {
            {0,0,0,0,0,0,0,0,0,0,0,0,0},
            {2.0/27.0,0,0,0,0,0,0,0,0,0,0,0,0},
            {1.0/36.0,1.0/12.0,0,0,0,0,0,0,0,0,0,0,0},
            {1.0/24.0,0,1.0/8.0,0,0,0,0,0,0,0,0,0,0},
            {5.0/12.0,0,-25.0/16.0,25.0/16.0,0,0,0,0,0,0,0,0,0},
            {1.0/20.0,0,0,1.0/4.0,1.0/5.0,0,0,0,0,0,0,0,0},
            {-25.0/108.0,0,0,125.0/108.0,-65.0/27.0,125.0/54.0,0,0,0,0,0,0,0},
            {31.0/300.0,0,0,0,61.0/225.0,-2.0/9.0,13.0/900.0,0,0,0,0,0,0},
            {2.0,0,0,-53.0/6.0,704.0/45.0,-107.0/9.0,67.0/90.0,3.0,0,0,0,0,0},
            {-91.0/108.0,0,0,23.0/108.0,-976.0/135.0,311.0/54.0,-19.0/60.0,17.0/6.0,-1.0/12.0,0,0,0,0},
            {2383.0/4100.0,0,0,-341.0/164.0,4496.0/1025.0,-301.0/82.0,2133.0/4100.0,45.0/82.0,45.0/164.0,18.0/41.0,0,0,0},
            {3.0/205.0,0,0,0,0,-6.0/41.0,-3.0/205.0,-3.0/41.0,3.0/41.0,6.0/41.0,0,0,0},
            {-1777.0/4100.0,0,0,-341.0/164.0,4496.0/1025.0,-289.0/82.0,2193.0/4100.0,51.0/82.0,33.0/164.0,12.0/41.0,0,1.0,0},
        };
        const double C[NS] = {
            0, 2.0/27.0, 1.0/9.0, 1.0/6.0, 5.0/12.0, 1.0/2.0, 5.0/6.0,
            1.0/6.0, 2.0/3.0, 1.0/3.0, 1.0, 0, 1.0,
        };
        // 8th-order solution weights (the solution is advanced with these).
        const double B8[NS] = {
            0,0,0,0,0, 34.0/105.0, 9.0/35.0, 9.0/35.0, 9.0/280.0, 9.0/280.0, 0, 41.0/840.0, 41.0/840.0,
        };
        // Leading-error estimator: h*(41/840)*(k0 + k10 - k11 - k12).
        constexpr double E_COEF = 41.0 / 840.0;
    }

    double NumericalPropagator::julianFromTimePoint(const TimePoint& t) {
        double secs = std::chrono::duration<double>(t.time_since_epoch()).count();
        return 2440587.5 + secs / 86400.0; // Unix epoch = JD 2440587.5 (UTC, no leap secs)
    }

    NumericalPropagator::NumericalPropagator(const libsgp4::Tle& tle,
                                             const ForceParams& fparams,
                                             const IntegratorParams& iparams)
        : force_(fparams), ip_(iparams), h_hint_(iparams.init_step_sec) {
        try {
            libsgp4::SGP4 sgp4(tle);
            libsgp4::DateTime epoch = tle.Epoch();
            jd_epoch_ = epoch.ToJulian();
            // TLE -> osculating state vector: evaluate SGP4 at the epoch itself.
            libsgp4::Eci eci = sgp4.FindPosition(epoch);
            libsgp4::Vector p = eci.Position();
            libsgp4::Vector v = eci.Velocity();
            r0_ = {p.x, p.y, p.z};
            v0_ = {v.x, v.y, v.z};
            r_cur_ = r0_;
            v_cur_ = v0_;
            t_cur_ = 0.0;
            valid_ = true;
        } catch (const std::exception& e) {
            Logger::log(std::string("NumericalPropagator init failed: ") + e.what());
            valid_ = false;
        } catch (...) {
            valid_ = false;
        }
    }

    void NumericalPropagator::rkStep(double t, double h, const Vector3& r, const Vector3& v,
                                     Vector3& r_out, Vector3& v_out, double& err_norm) const {
        Vector3 kr[NS], kv[NS];
        for (int i = 0; i < NS; ++i) {
            Vector3 rr = r, vv = v;
            for (int j = 0; j < i; ++j) {
                double aij = A[i][j];
                if (aij != 0.0) {
                    double f = h * aij;
                    rr = rr + kr[j] * f;
                    vv = vv + kv[j] * f;
                }
            }
            double jd = jd_epoch_ + (t + C[i] * h) / 86400.0;
            kr[i] = vv;                              // dr/dt = v
            kv[i] = force_.acceleration(jd, rr, vv); // dv/dt = a
        }
        r_out = r;
        v_out = v;
        for (int i = 0; i < NS; ++i) {
            if (B8[i] != 0.0) {
                double f = h * B8[i];
                r_out = r_out + kr[i] * f;
                v_out = v_out + kv[i] * f;
            }
        }
        // Embedded error estimate.
        double he = h * E_COEF;
        Vector3 er = (kr[0] + kr[10] - kr[11] - kr[12]) * he;
        Vector3 ev = (kv[0] + kv[10] - kv[11] - kv[12]) * he;
        double comp[6] = {er.x, er.y, er.z, ev.x, ev.y, ev.z};
        double yn[6]   = {r_out.x, r_out.y, r_out.z, v_out.x, v_out.y, v_out.z};
        double y0[6]   = {r.x, r.y, r.z, v.x, v.y, v.z};
        double s2 = 0.0;
        for (int i = 0; i < 6; ++i) {
            double sc = ip_.atol + ip_.rtol * std::max(std::fabs(yn[i]), std::fabs(y0[i]));
            double e = comp[i] / sc;
            s2 += e * e;
        }
        err_norm = std::sqrt(s2 / 6.0);
    }

    void NumericalPropagator::integrateTo(double t_target) {
        if (!valid_) return;
        // Start from whichever cached checkpoint is closer to the target.
        double t;
        Vector3 r, v;
        if (std::fabs(t_target - t_cur_) <= std::fabs(t_target)) {
            t = t_cur_; r = r_cur_; v = v_cur_;
        } else {
            t = 0.0; r = r0_; v = v0_;
        }

        const double dir = (t_target >= t) ? 1.0 : -1.0;
        double natural_h = std::max(h_hint_, ip_.min_step_sec); // unclamped adaptive magnitude
        int guard = 0;
        while (dir * (t_target - t) > 1e-9) {
            double rem = dir * (t_target - t);
            double hmag = std::min(natural_h, rem);
            double h = dir * hmag;

            Vector3 r_new, v_new;
            double err;
            rkStep(t, h, r, v, r_new, v_new, err);

            bool at_floor = (hmag <= ip_.min_step_sec * 1.0000001);
            if (err <= 1.0 || at_floor) {
                t += h; r = r_new; v = v_new;
                double s = (err > 0.0) ? 0.9 * std::pow(err, -0.125) : 5.0;
                s = std::min(5.0, std::max(0.2, s));
                natural_h = std::min(natural_h * s, ip_.max_step_sec);
                h_hint_ = natural_h;
            } else {
                double s = std::max(0.2, 0.9 * std::pow(err, -0.125));
                natural_h = std::max(natural_h * s, ip_.min_step_sec);
            }
            if (++guard > 500000) {
                Logger::log("NumericalPropagator: step guard tripped");
                break;
            }
        }
        t_cur_ = t; r_cur_ = r; v_cur_ = v;
    }

    std::pair<Vector3, Vector3> NumericalPropagator::stateAtSeconds(double t_sec) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!valid_) return {{0, 0, 0}, {0, 0, 0}};
        integrateTo(t_sec);
        return {r_cur_, v_cur_};
    }

    std::pair<Vector3, Vector3> NumericalPropagator::propagate(const TimePoint& t) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!valid_) return {{0, 0, 0}, {0, 0, 0}};
        double t_sec = (julianFromTimePoint(t) - jd_epoch_) * 86400.0;
        integrateTo(t_sec);
        return {r_cur_, v_cur_};
    }
}
