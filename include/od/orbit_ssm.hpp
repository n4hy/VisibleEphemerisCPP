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

    // Diagonal process-noise covariance for the interval ending at t_k_sec.
    // Continuous-time model:
    //     dot(r) = v
    //     dot(v) = a(r,v,t) + w_a          w_a ~ N(0, q_a^2 I_3)   [km/s^2]
    //     dot(bc) = b_dot                                          [Hz]
    //     dot(bd) = w_b                    w_b ~ N(0, q_b^2)       [Hz/s]
    //
    // Discretising with a simple Euler-white-noise-integral gives:
    //     Q_v  = q_a^2 * dt * I_3          [km^2/s^2] (variance on v)
    //     Q_bc = q_bc^2 * dt               [Hz^2]
    //     Q_bd = q_bd^2 * dt               [(Hz/s)^2]
    //     Q_r  = 0                         (position driven by velocity)
    //
    // This is a *simple* Q; a Van Loan discretisation would produce cross
    // (r-v) blocks. Kept diagonal in v0 as documented in docs §7.3.
    StateMat Q(float t_k_sec) const override {
        StateMat Qm = StateMat::Zero();
        const double dt = static_cast<double>(t_k_sec) - static_cast<double>(t_prev_sec_);
        const double dt_abs = std::abs(dt);
        const double q_a  = cfg_.sigma_process_accel;
        const double q_bc = cfg_.sigma_process_bc;
        const double q_bd = cfg_.sigma_process_bd;
        const float v_var  = static_cast<float>(q_a  * q_a  * dt_abs);
        const float bc_var = static_cast<float>(q_bc * q_bc * dt_abs);
        const float bd_var = static_cast<float>(q_bd * q_bd * dt_abs);
        Qm(idx::VX, idx::VX) = v_var;
        Qm(idx::VY, idx::VY) = v_var;
        Qm(idx::VZ, idx::VZ) = v_var;
        Qm(idx::BC, idx::BC) = bc_var;
        Qm(idx::BD, idx::BD) = bd_var;
        // A tiny position regulariser keeps the sqrt-Q Cholesky in NLF's
        // SRUKF non-singular. Chosen as 1e-6 km^2 (1 mm^2) -- below numerical
        // noise but nonzero.
        Qm(idx::RX, idx::RX) = 1e-6f;
        Qm(idx::RY, idx::RY) = 1e-6f;
        Qm(idx::RZ, idx::RZ) = 1e-6f;
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
