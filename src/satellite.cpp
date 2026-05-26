// satellite.cpp - Satellite orbital-model implementation.
// The constructor parses the TLE and builds an SGP4 propagator (synthetic
// SUN/MOON objects get reserved negative ids). enableHighPrecision() lazily
// attaches the numerical HPOP backend, seeding its drag model with the TLE B*.
// propagate() and getGeodetic() dispatch to HPOP when present, otherwise SGP4;
// HPOP geodetic reuses libsgp4's WGS84 ECI->geodetic conversion so both paths
// agree. calculateGroundTrack()/passes provide the cached display data.
#include "satellite.hpp"
#include "logger.hpp"
#include <iostream>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <CoordTopocentric.h>
#include <CoordGeodetic.h>

namespace ve {
    Satellite::Satellite(std::string name, std::string line1, std::string line2) 
        : name_(std::move(name)), is_computing(false) {
        try {
            tle_object_ = std::make_unique<libsgp4::Tle>(name_, line1, line2);
            sgp4_object_ = std::make_unique<libsgp4::SGP4>(*tle_object_);

            // Allow override for synthetic objects
            if (name_ == "SUN") norad_id_ = -1;
            else if (name_ == "MOON") norad_id_ = -2;
            else norad_id_ = static_cast<int>(tle_object_->NoradNumber());

        } catch (const std::exception& e) {
            Logger::log("Satellite TLE parse error for '" + name_ + "': " + e.what());
            norad_id_ = 0;
        } catch (...) {
            Logger::log("Satellite TLE parse error for '" + name_ + "': unknown exception");
            norad_id_ = 0;
        }
    }

    Satellite::Satellite(Satellite&& other) noexcept
        : name_(std::move(other.name_)),
          norad_id_(other.norad_id_),
          tle_object_(std::move(other.tle_object_)),
          sgp4_object_(std::move(other.sgp4_object_)),
          hpop_(std::move(other.hpop_)),
          full_track_(std::move(other.full_track_)),
          predicted_passes_(std::move(other.predicted_passes_))
    {
        is_computing.store(std::atomic_exchange(&other.is_computing, false));
    }

    int Satellite::getTleEpochYear() const { return tle_object_ ? tle_object_->Epoch().Year() : 0; }
    double Satellite::getTleEpochDay() const {
        if (!tle_object_) return 0.0;
        try {
            auto dt = tle_object_->Epoch();
            std::tm t = {}; t.tm_year = dt.Year() - 1900; t.tm_mon = dt.Month() - 1; t.tm_mday = dt.Day();
            t.tm_hour = dt.Hour(); t.tm_min = dt.Minute(); t.tm_sec = dt.Second();
            mktime(&t); 
            return (t.tm_yday + 1) + (dt.Hour() + dt.Minute()/60.0 + dt.Second()/3600.0) / 24.0;
        } catch(...) { return 0.0; }
    }
    
    double Satellite::getApogeeKm() const {
        if (!tle_object_) return 0.0;
        try {
            double mm = tle_object_->MeanMotion();
            double n = mm * 2.0 * PI / SECONDS_PER_DAY;
            double a = std::pow(EARTH_MU / (n*n), 1.0/3.0);
            double e = tle_object_->Eccentricity();
            return a * (1 + e) - EARTH_RADIUS_KM;
        } catch(...) { return 0.0; }
    }

    void Satellite::enableHighPrecision(ForceParams fp, const IntegratorParams& ip) {
        if (!tle_object_) return;
        std::lock_guard<std::mutex> lock(sat_mutex_);
        fp.bstar = tle_object_->BStar();   // drag ballistic coefficient source
        auto hp = std::make_unique<NumericalPropagator>(*tle_object_, fp, ip);
        if (hp->valid()) {
            hpop_ = std::move(hp);
        } else {
            Logger::log("HPOP init failed for '" + name_ + "'; staying on SGP4");
        }
    }

    std::pair<Vector3, Vector3> Satellite::propagate(const TimePoint& t) const {
        // High-precision path: the numerical propagator is internally synchronized.
        if (hpop_) return hpop_->propagate(t);
        if (!sgp4_object_) return {{0,0,0},{0,0,0}};
        try {
            std::lock_guard<std::mutex> lock(sat_mutex_);
            std::time_t tt = Clock::to_time_t(t);
            std::tm gmt;
            gmtime_r(&tt, &gmt);
            libsgp4::DateTime dt(gmt.tm_year + 1900, gmt.tm_mon + 1, gmt.tm_mday, gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
            libsgp4::Eci eci = sgp4_object_->FindPosition(dt);
            libsgp4::Vector pos = eci.Position(); libsgp4::Vector vel = eci.Velocity();
            return {{pos.x, pos.y, pos.z}, {vel.x, vel.y, vel.z}};
        } catch (...) { return {{0,0,0},{0,0,0}}; }
    }

    Geodetic Satellite::getGeodetic(const TimePoint& t) const {
        std::time_t tt = Clock::to_time_t(t);
        std::tm gmt;
        gmtime_r(&tt, &gmt);
        libsgp4::DateTime dt(gmt.tm_year + 1900, gmt.tm_mon + 1, gmt.tm_mday, gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
        if (hpop_) {
            // Integrate to t, then reuse libsgp4's WGS84 ECI->geodetic conversion
            // (identical method to the SGP4 path) on the numerical position.
            try {
                auto [r, v] = hpop_->propagate(t);
                (void)v;
                libsgp4::Eci eci(dt, libsgp4::Vector(r.x, r.y, r.z));
                libsgp4::CoordGeodetic geo = eci.ToGeodetic();
                return { geo.latitude * RAD2DEG, geo.longitude * RAD2DEG, geo.altitude };
            } catch (...) { return {0,0,0}; }
        }
        if (!sgp4_object_) return {0,0,0};
        try {
            std::lock_guard<std::mutex> lock(sat_mutex_);
            libsgp4::Eci eci = sgp4_object_->FindPosition(dt);
            libsgp4::CoordGeodetic geo = eci.ToGeodetic();
            return { geo.latitude * RAD2DEG, geo.longitude * RAD2DEG, geo.altitude };
        } catch (...) { return {0,0,0}; }
    }

    void Satellite::calculateGroundTrack(const TimePoint& now, int half_width_mins, int step_secs) {
        std::vector<Geodetic> new_track;
        TimePoint start = now - std::chrono::minutes(half_width_mins);
        TimePoint end = now + std::chrono::minutes(half_width_mins);
        TimePoint t = start;
        // Safety reserve
        new_track.reserve((half_width_mins * 2 * 60 / step_secs) + 5);
        
        while(t <= end) {
            Geodetic g = getGeodetic(t);
            // Filter out decay errors (0,0,0)
            if (!(std::abs(g.lat_deg) < 0.001 && std::abs(g.alt_km) < 0.001)) {
                new_track.push_back(g);
            }
            t += std::chrono::seconds(step_secs);
        }
        std::lock_guard<std::mutex> lock(sat_mutex_);
        full_track_ = std::move(new_track);
    }

    std::vector<Geodetic> Satellite::getFullTrackCopy() const {
        std::lock_guard<std::mutex> lock(sat_mutex_);
        return full_track_;
    }

    void Satellite::setPredictedPasses(const std::vector<PassEvent>& passes) {
        std::lock_guard<std::mutex> lock(sat_mutex_);
        predicted_passes_ = passes;
    }

    std::vector<Satellite::PassEvent> Satellite::getPredictedPasses() const {
        std::lock_guard<std::mutex> lock(sat_mutex_);
        return predicted_passes_;
    }
}
