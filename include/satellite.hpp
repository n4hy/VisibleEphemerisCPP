// satellite.hpp - Per-satellite orbital model.
//
// Wraps an libsgp4 TLE/SGP4 propagator and, optionally, the high-precision
// numerical propagator (HPOP, see numerical_propagator.hpp). propagate() and
// getGeodetic() transparently dispatch to whichever backend is active, so the
// rest of the application is agnostic to the propagation method. Also owns the
// per-satellite ground-track and predicted-pass caches. All accessors are
// thread-safe (sat_mutex_); the HPOP backend is independently synchronized.
#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <deque>
#include <atomic>
#include <mutex>
#include <Tle.h>
#include <SGP4.h>
#include <Eci.h>
#include "numerical_propagator.hpp"

namespace ve {
    class Satellite {
    public:
        Satellite(std::string name, std::string line1, std::string line2);
        Satellite(Satellite&& other) noexcept;
        Satellite(const Satellite&) = delete;
        Satellite& operator=(const Satellite&) = delete;

        std::pair<Vector3, Vector3> propagate(const TimePoint& t) const;
        Geodetic getGeodetic(const TimePoint& t) const;

        // Switch this satellite to the high-precision numerical propagator. The
        // force-model parameters carry degree/order and perturbation switches;
        // the TLE B* term is filled in automatically for the drag model.
        void enableHighPrecision(ForceParams fp = ForceParams{},
                                 const IntegratorParams& ip = IntegratorParams{});
        bool isHighPrecision() const { return hpop_ != nullptr; }

        const std::string& getName() const { return name_; }
        int getNoradId() const { return norad_id_; }
        int getTleEpochYear() const;
        double getTleEpochDay() const;
        double getApogeeKm() const;

        void calculateGroundTrack(const TimePoint& now, int half_width_mins, int step_secs = 60);
        std::vector<Geodetic> getFullTrackCopy() const;

        struct PassEvent { TimePoint time; bool is_aos; };
        void setPredictedPasses(const std::vector<PassEvent>& passes);
        std::vector<PassEvent> getPredictedPasses() const;

        std::atomic<bool> is_computing;

    private:
        std::string name_;
        int norad_id_;
        std::unique_ptr<libsgp4::Tle> tle_object_;
        std::unique_ptr<libsgp4::SGP4> sgp4_object_;
        // Optional high-precision numerical propagator; when set, replaces SGP4.
        std::unique_ptr<NumericalPropagator> hpop_;
        // Mutex for thread-safe access to SGP4 and cached data
        mutable std::mutex sat_mutex_;
        std::vector<Geodetic> full_track_; 
        std::vector<PassEvent> predicted_passes_;
    };
}
