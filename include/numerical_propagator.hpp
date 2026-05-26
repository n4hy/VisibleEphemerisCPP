#pragma once
#include "types.hpp"
#include "force_model.hpp"
#include <Tle.h>
#include <utility>
#include <mutex>

// High-precision numerical orbit propagator (HPOP).
//
// Initial conditions are obtained the only correct way for a TLE: the SGP4
// theory is evaluated AT the element-set epoch to produce an osculating ECI
// (TEME) state vector (r0, v0). That state vector seeds an adaptive Fehlberg
// RK7(8) integration of the full force model (see force_model.hpp). The output
// frame matches SGP4's, so existing observer/geodetic code is unchanged.
namespace ve {

    struct IntegratorParams {
        double rtol = 1e-10;          // relative error tolerance
        double atol = 1e-9;           // absolute floor (km, km/s)
        double init_step_sec = 60.0;
        double min_step_sec  = 1e-3;
        double max_step_sec  = 300.0;
    };

    class NumericalPropagator {
    public:
        NumericalPropagator(const libsgp4::Tle& tle,
                            const ForceParams& fparams = ForceParams{},
                            const IntegratorParams& iparams = IntegratorParams{});

        // Propagate to absolute UTC time; returns ECI position (km) and velocity (km/s).
        std::pair<Vector3, Vector3> propagate(const TimePoint& t);
        // Lower-level: state at a number of seconds since the TLE epoch.
        std::pair<Vector3, Vector3> stateAtSeconds(double t_sec);

        double epochJulian() const { return jd_epoch_; }
        Vector3 epochPosition() const { return r0_; }
        Vector3 epochVelocity() const { return v0_; }
        const ForceModel& forceModel() const { return force_; }
        bool valid() const { return valid_; }

        static double julianFromTimePoint(const TimePoint& t);

    private:
        void rkStep(double t, double h, const Vector3& r, const Vector3& v,
                    Vector3& r_out, Vector3& v_out, double& err_norm) const;
        void integrateTo(double t_target_sec); // assumes mtx_ held

        ForceModel force_;
        IntegratorParams ip_;
        double jd_epoch_ = 0.0;
        Vector3 r0_{0, 0, 0}, v0_{0, 0, 0};   // anchor state at epoch
        double t_cur_ = 0.0;                   // seconds since epoch of cached state
        Vector3 r_cur_{0, 0, 0}, v_cur_{0, 0, 0};
        double h_hint_ = 60.0;                 // adaptive step carried between calls
        bool valid_ = false;
        mutable std::mutex mtx_;
    };
}
