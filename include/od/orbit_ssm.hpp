// orbit_ssm.hpp -- StateSpaceModel<8, 1> subclass that binds the NLF SRUKF to
// the VisibleEphemeris ForceModel + Doppler measurement.
//
// LOAD-BEARING CONVENTION (Newton note):
//
//   NLF's SRUKF calls   model.f(sigma_i, t_k, u_k)   17 times inside a single
//   predict(t_k, u_k). Every call must integrate from THE SAME previous epoch
//   t_prev to t_k. We can NOT infer t_prev from the SRUKF-supplied t_k; the
//   driver stores it on the model between predicts:
//
//       model.set_t_prev_sec(t_prev);
//       cross = srukf.predict(t_k, u);    // all 17 sigma points see this t_prev
//       model.set_t_prev_sec(t_k);        // done AFTER predict returns
//       srukf.update(t_k, y);
//
//   Q(t_k) similarly needs dt = t_k - t_prev; it reads the stored t_prev.
//
//   This coupling is intentional and confined to the driver in od_smoother.cpp.

#pragma once

#include "StateSpaceModel.h"          // NLF/UKFModel::StateSpaceModel
#include "od/od_types.hpp"
#include "od/od_dynamics.hpp"
#include "od/doppler_measurement.hpp"

#include "force_model.hpp"
#include "types.hpp"

#include <memory>

namespace ve::od {

class OrbitStateSpaceModel : public UKFModel::StateSpaceModel<OD_STATE_DIM, OD_OBS_DIM> {
public:
    using Base = UKFModel::StateSpaceModel<OD_STATE_DIM, OD_OBS_DIM>;
    using State       = Base::State;         // Eigen::Matrix<float, 8, 1>
    using Observation = Base::Observation;   // Eigen::Matrix<float, 1, 1>
    using StateMat    = Base::StateMat;      // Eigen::Matrix<float, 8, 8>
    using ObsMat      = Base::ObsMat;        // Eigen::Matrix<float, 1, 1>

    // Construct with a shared ForceModel (owned externally) and the filter
    // configuration. `jd_at_t_ref` is the Julian Date corresponding to
    // t_pass_sec = 0 (i.e. the pass-AOS UTC epoch).
    OrbitStateSpaceModel(const ve::ForceModel& fm,
                         const FilterConfig& cfg,
                         double jd_at_t_ref)
        : fm_(fm), cfg_(cfg), jd_at_t_ref_(jd_at_t_ref), t_prev_sec_(0.0f) {}

    // ---- Driver-owned bookkeeping (see header comment) ---------------------

    void set_t_prev_sec(float t_prev_sec) { t_prev_sec_ = t_prev_sec; }
    float t_prev_sec() const { return t_prev_sec_; }

    double jd_at_t_ref() const { return jd_at_t_ref_; }
    const FilterConfig& config() const { return cfg_; }

    // ---- NLF StateSpaceModel virtual overrides ----------------------------

    // Dynamics: propagate one sigma point from t_prev_sec_ to t_k_sec using
    // RK4 through the ForceModel. Bias states integrate trivially.
    State f(const State& x_prev, float t_k_sec,
            const Eigen::Ref<const State>& /*u_k*/) const override {
        // Widen to double for the integrator (float km-scale precision is
        // ~1 mm, adequate; we widen for consistency with ForceModel's double
        // interior).
        const ve::Vector3 r0{ static_cast<double>(x_prev(idx::RX)),
                              static_cast<double>(x_prev(idx::RY)),
                              static_cast<double>(x_prev(idx::RZ)) };
        const ve::Vector3 v0{ static_cast<double>(x_prev(idx::VX)),
                              static_cast<double>(x_prev(idx::VY)),
                              static_cast<double>(x_prev(idx::VZ)) };

        const double t_prev = static_cast<double>(t_prev_sec_);
        const double t_end  = static_cast<double>(t_k_sec);

        auto [r1, v1] = integrate_rk4(fm_, jd_at_t_ref_, t_prev,
                                      r0, v0, t_end);

        // Oscillator: b_c += b_dot * dt,  b_dot unchanged.
        const double dt = t_end - t_prev;
        const double bc  = static_cast<double>(x_prev(idx::BC))
                         + static_cast<double>(x_prev(idx::BD)) * dt;
        const double bd  = static_cast<double>(x_prev(idx::BD));

        State x_next;
        x_next(idx::RX) = static_cast<float>(r1.x);
        x_next(idx::RY) = static_cast<float>(r1.y);
        x_next(idx::RZ) = static_cast<float>(r1.z);
        x_next(idx::VX) = static_cast<float>(v1.x);
        x_next(idx::VY) = static_cast<float>(v1.y);
        x_next(idx::VZ) = static_cast<float>(v1.z);
        x_next(idx::BC) = static_cast<float>(bc);
        x_next(idx::BD) = static_cast<float>(bd);
        return x_next;
    }

    // Measurement: h(x, t) = predicted Doppler at t (UTC = t_ref + t_sec).
    Observation h(const State& x_k, float t_k_sec) const override {
        StateVecD x_d;
        for (int i = 0; i < OD_STATE_DIM; ++i) x_d(i) = static_cast<double>(x_k(i));

        // Reconstruct the UTC epoch for this measurement from t_ref + seconds.
        const ve::TimePoint t_utc = cfg_.t_ref_utc
            + std::chrono::duration_cast<ve::Clock::duration>(
                std::chrono::duration<double>(static_cast<double>(t_k_sec)));

        const double f_hz = predict_doppler_hz(
            x_d, t_utc, cfg_.f_transmit_hz, cfg_.station, cfg_.t_ref_utc);
        Observation y;
        y(0) = static_cast<float>(f_hz);
        return y;
    }

    // Process-noise covariance for the interval ending at t_k_sec.
    //
    // Continuous-time model (per Cartesian axis, three copies):
    //     dot(r) = v
    //     dot(v) = a(r,v,t) + w_a          w_a ~ WSS white,
    //                                       PSD q_a^2  [km^2 / s^3]
    //     dot(bc) = b_dot                                          [Hz]
    //     dot(bd) = w_b                    w_b ~ WSS white,
    //                                       PSD q_bd^2 [(Hz/s)^2 / s]
    //
    // Van Loan / closed-form discretisation of the double-integrator block
    // gives, per axis (Bar-Shalom, Estimation with Applications to Tracking
    // and Navigation, Eq. 6.3.2-4):
    //
    //     Q_rr = q_a^2 * dt^3 / 3
    //     Q_rv = q_a^2 * dt^2 / 2
    //     Q_vv = q_a^2 * dt
    //
    // The earlier v0 implementation zeroed Q_rr and Q_rv and added a fixed
    // 1e-6 km^2 position regulariser (see Newton audit); that under-modelled
    // position uncertainty in a way that grew with dt. Full Van Loan closes
    // the algebraic gap. The r-v cross term is set symmetrically so the
    // resulting sub-block is PD for any dt > 0 (its determinant per axis is
    // q_a^4 * dt^4 / 12).
    //
    // dt is taken as the absolute-value difference; the interval-sign is
    // handled by the integrator, not by Q. Q is required to be PD, not
    // signed. For dt = 0 the block collapses to zero (no time has passed);
    // callers must not sample-and-hold on a zero-length interval.
    //
    // The oscillator bias/drift block remains simply Euler-discretised
    // (bc gets bc^2*dt from its white driver, bd similarly) since those
    // are scalar chains that the CV algebra does not couple to r,v.
    StateMat Q(float t_k_sec) const override {
        StateMat Qm = StateMat::Zero();
        const double dt = static_cast<double>(t_k_sec) - static_cast<double>(t_prev_sec_);
        const double dt_abs = std::abs(dt);
        const double q_a2  = cfg_.sigma_process_accel * cfg_.sigma_process_accel;
        const double q_bc2 = cfg_.sigma_process_bc    * cfg_.sigma_process_bc;
        const double q_bd2 = cfg_.sigma_process_bd    * cfg_.sigma_process_bd;

        // Constant-velocity double-integrator block (per Cartesian axis).
        const double dt2 = dt_abs * dt_abs;
        const double dt3 = dt2 * dt_abs;
        const float qrr = static_cast<float>(q_a2 * dt3 / 3.0);
        const float qrv = static_cast<float>(q_a2 * dt2 / 2.0);
        const float qvv = static_cast<float>(q_a2 * dt_abs);
        const int r_idx[3] = { idx::RX, idx::RY, idx::RZ };
        const int v_idx[3] = { idx::VX, idx::VY, idx::VZ };
        for (int i = 0; i < 3; ++i) {
            Qm(r_idx[i], r_idx[i]) = qrr;
            Qm(v_idx[i], v_idx[i]) = qvv;
            Qm(r_idx[i], v_idx[i]) = qrv;
            Qm(v_idx[i], r_idx[i]) = qrv;
        }
        Qm(idx::BC, idx::BC) = static_cast<float>(q_bc2 * dt_abs);
        Qm(idx::BD, idx::BD) = static_cast<float>(q_bd2 * dt_abs);
        return Qm;
    }

    // Measurement-noise covariance (1x1).
    ObsMat R(float /*t_k_sec*/) const override {
        ObsMat Rm;
        Rm(0, 0) = static_cast<float>(cfg_.sigma_R_hz * cfg_.sigma_R_hz);
        return Rm;
    }

    // No angular states or observations in the OD model.
    bool isAngularState(int) const override { return false; }
    bool isAngularObservation(int) const override { return false; }

private:
    const ve::ForceModel& fm_;
    FilterConfig cfg_;
    double jd_at_t_ref_;
    float  t_prev_sec_;
};

} // namespace ve::od
