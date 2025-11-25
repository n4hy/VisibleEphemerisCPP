#include "satellite.hpp"
#include <iostream>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <CoordTopocentric.h>
#include <CoordGeodetic.h>

namespace ve {
    Satellite::Satellite(std::string name, std::string line1, std::string line2) : name_(std::move(name)) {
        try {
            tle_object_ = std::make_unique<libsgp4::Tle>(name_, line1, line2);
            sgp4_object_ = std::make_unique<libsgp4::SGP4>(*tle_object_);
            norad_id_ = static_cast<int>(tle_object_->NoradNumber());
        } catch (...) { norad_id_ = 0; }
    }
    int Satellite::getTleEpochYear() const { return tle_object_ ? tle_object_->Epoch().Year() : 0; }
    double Satellite::getTleEpochDay() const {
        if (!tle_object_) return 0.0;
        auto dt = tle_object_->Epoch();
        std::tm t = {};
        t.tm_year = dt.Year() - 1900; t.tm_mon = dt.Month() - 1; t.tm_mday = dt.Day();
        t.tm_hour = dt.Hour(); t.tm_min = dt.Minute(); t.tm_sec = dt.Second();
        mktime(&t); 
        return (t.tm_yday + 1) + (dt.Hour() + dt.Minute()/60.0 + dt.Second()/3600.0) / 24.0;
    }
    
    double Satellite::getApogeeKm() const {
        if (!tle_object_) return 0.0;
        // Mean Motion: Revs per Day
        double mm = tle_object_->MeanMotion(); 
        // Convert to Rad/Sec: (mm * 2PI) / 86400
        double n = mm * 2.0 * PI / 86400.0; 
        // Earth Gravitational Param (km^3/s^2)
        double mu = 398600.4418; 
        // Semi-major axis (km)
        double a = std::pow(mu / (n*n), 1.0/3.0); 
        double e = tle_object_->Eccentricity();
        
        // Apogee Radius = a * (1 + e)
        // Altitude = Radius - EarthRadius
        return (a * (1 + e)) - EARTH_RADIUS_KM;
    }

    std::pair<Vector3, Vector3> Satellite::propagate(const TimePoint& t) const {
        if (!sgp4_object_) return {{0,0,0},{0,0,0}};
        std::time_t tt = Clock::to_time_t(t);
        std::tm* gmt = std::gmtime(&tt);
        libsgp4::DateTime dt(gmt->tm_year + 1900, gmt->tm_mon + 1, gmt->tm_mday, gmt->tm_hour, gmt->tm_min, gmt->tm_sec);
        libsgp4::Eci eci = sgp4_object_->FindPosition(dt);
        libsgp4::Vector pos = eci.Position(); libsgp4::Vector vel = eci.Velocity();
        return {{pos.x, pos.y, pos.z}, {vel.x, vel.y, vel.z}};
    }
    
    Geodetic Satellite::getGeodetic(const TimePoint& t) const {
        if (!sgp4_object_) return {0,0,0};
        std::time_t tt = Clock::to_time_t(t);
        std::tm* gmt = std::gmtime(&tt);
        libsgp4::DateTime dt(gmt->tm_year + 1900, gmt->tm_mon + 1, gmt->tm_mday, gmt->tm_hour, gmt->tm_min, gmt->tm_sec);
        libsgp4::Eci eci = sgp4_object_->FindPosition(dt);
        libsgp4::CoordGeodetic geo = eci.ToGeodetic();
        return { geo.latitude * RAD2DEG, geo.longitude * RAD2DEG, geo.altitude };
    }
}
