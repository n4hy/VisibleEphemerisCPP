#include "pass_predictor.hpp"
namespace ve {
    PassPredictor::PassPredictor(const Observer& obs) : observer_(obs) {}
    std::vector<PassDetails> PassPredictor::predict(const Satellite& sat, const TimePoint& start, const TimePoint& end) {
        std::vector<PassDetails> passes;
        auto step = std::chrono::minutes(1);
        bool was_above = (observer_.calculateLookAngle(sat.propagate(start).first, start).elevation > 0);
        TimePoint t = start;
        PassDetails p;
        if (was_above) p.aos = start;

        while (t < end) {
            TimePoint next = t + step;
            bool is_above = (observer_.calculateLookAngle(sat.propagate(next).first, next).elevation > 0);
            if (!was_above && is_above) {
                p.sat_id = sat.getNoradId(); p.aos = t; 
                p.max_el_deg = 0;
            } else if (was_above && !is_above) {
                p.los = t; 
                p.max_el_time = p.aos + (p.los - p.aos)/2;
                auto [pos, vel] = sat.propagate(p.max_el_time);
                p.max_el_deg = observer_.calculateLookAngle(pos, p.max_el_time).elevation;
                auto state = VisibilityCalculator::calculateState(pos, observer_.getPositionECI(p.max_el_time), p.max_el_time, p.max_el_deg);
                p.is_visible = (state == VisibilityCalculator::State::VISIBLE);
                passes.push_back(p);
            }
            was_above = is_above; t = next;
        }
        return passes;
    }
}
