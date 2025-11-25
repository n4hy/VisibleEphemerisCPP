#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <Tle.h>
#include <SGP4.h>
#include <Eci.h>

namespace ve {
    class Satellite {
    public:
        Satellite(std::string name, std::string line1, std::string line2);
        Satellite(Satellite&&) = default;
        Satellite& operator=(Satellite&&) = default;

        std::pair<Vector3, Vector3> propagate(const TimePoint& t) const;
        Geodetic getGeodetic(const TimePoint& t) const;

        const std::string& getName() const { return name_; }
        int getNoradId() const { return norad_id_; }
        int getTleEpochYear() const;
        double getTleEpochDay() const;
        
        double getApogeeKm() const;
        
    private:
        std::string name_;
        int norad_id_;
        std::unique_ptr<libsgp4::Tle> tle_object_;
        std::unique_ptr<libsgp4::SGP4> sgp4_object_;
    };
}
