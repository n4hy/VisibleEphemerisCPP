#pragma once
#include "satellite.hpp"
#include "observer.hpp"
#include "visibility.hpp"
#include <vector>

namespace ve {
    struct PassDetails {
        int sat_id;
        TimePoint aos; TimePoint los; TimePoint max_el_time;
        double max_el_deg; bool is_visible;
    };
    class PassPredictor {
    public:
        PassPredictor(const Observer& obs);
        std::vector<PassDetails> predict(const Satellite& sat, const TimePoint& start, const TimePoint& end);
    private:
        Observer observer_;
        TimePoint findCrossing(const Satellite& sat, TimePoint t_guess, double target_el, bool rising);
        double getElevationAt(const Satellite& sat, const TimePoint& t);
    };
}
