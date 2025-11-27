#include "pass_predictor.hpp"
#include <iostream>

namespace ve {
    PassPredictor::PassPredictor(const Observer& obs) : observer_(obs) {}

    double PassPredictor::getElevation(const Satellite& sat, const TimePoint& t) {
        auto [pos, vel] = sat.propagate(t);
        return observer_.calculateLookAngle(pos, t).elevation;
    }

    TimePoint PassPredictor::solveNewton(const Satellite& sat, TimePoint initial_guess) {
        TimePoint t = initial_guess;
        double epsilon = 0.01; 
        int max_iter = 10;
        for(int i=0; i<max_iter; ++i) {
            double el = getElevation(sat, t);
            if (std::abs(el) < epsilon) return t;
            TimePoint t_plus = t + std::chrono::seconds(1);
            double el_plus = getElevation(sat, t_plus);
            double deriv = (el_plus - el); 
            if (std::abs(deriv) < 1e-5) break; 
            double delta_sec = el / deriv;
            if (delta_sec > 600) delta_sec = 600; if (delta_sec < -600) delta_sec = -600;
            t = t - std::chrono::milliseconds((long)(delta_sec * 1000));
        }
        return t;
    }

    std::vector<Satellite::PassEvent> PassPredictor::predict(Satellite& sat, const TimePoint& start, int search_window_mins) {
        std::vector<Satellite::PassEvent> results;
        TimePoint t = start;
        TimePoint end = start + std::chrono::minutes(search_window_mins);
        auto step = std::chrono::minutes(2); 
        double prev_el = getElevation(sat, t);
        
        while (t < end) {
            TimePoint next_t = t + step;
            double next_el = getElevation(sat, next_t);
            
            if ((prev_el < 0 && next_el >= 0) || (prev_el >= 0 && next_el < 0)) {
                TimePoint crossing = solveNewton(sat, t + step/2);
                double el_check = getElevation(sat, crossing + std::chrono::seconds(1));
                double el_at = getElevation(sat, crossing);
                double slope = el_check - el_at;
                results.push_back({crossing, (slope > 0)});
            }
            prev_el = next_el;
            t = next_t;
        }
        return results;
    }
}
