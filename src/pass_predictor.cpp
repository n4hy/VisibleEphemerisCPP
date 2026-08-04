// pass_predictor.cpp - AOS/LOS pass prediction implementation.
// Strategy: scan the search window in coarse 2-minute steps tracking the
// satellite's elevation; whenever the elevation sign flips (a horizon crossing)
// refine the exact time with Newton's method, then classify it as AOS (rising)
// or LOS (setting) from the local slope. getElevation() routes through
// Satellite::propagate(), so prediction is correct for SGP4 and HPOP alike.
#include "pass_predictor.hpp"
#include <iostream>

namespace ve {
    PassPredictor::PassPredictor(const Observer& obs) : observer_(obs) {}

    // Elevation (deg) of the satellite above the observer's horizon at time t.
    double PassPredictor::getElevation(const Satellite& sat, const TimePoint& t) {
        auto [pos, vel] = sat.propagate(t);
        return observer_.calculateLookAngle(pos, t).elevation;
    }

    // Refine a bracketed horizon crossing (elevation == 0) by Newton's method,
    // estimating the derivative with a 1-second forward difference. The per-step
    // correction is clamped to +/-600 s to keep a bad derivative from diverging.
    TimePoint PassPredictor::solveNewton(const Satellite& sat, TimePoint initial_guess) {
        TimePoint t = initial_guess;
        double epsilon = 0.01;   // converged when |elevation| < 0.01 deg
        int max_iter = 10;
        for(int i=0; i<max_iter; ++i) {
            double el = getElevation(sat, t);
            if (std::abs(el) < epsilon) return t;
            TimePoint t_plus = t + std::chrono::seconds(1);
            double el_plus = getElevation(sat, t_plus);
            double deriv = (el_plus - el);            // deg per second
            if (std::abs(deriv) < 1e-5) break;        // flat: cannot improve
            double delta_sec = el / deriv;
            if (delta_sec >  600) delta_sec =  600;
            if (delta_sec < -600) delta_sec = -600;
            t = t - std::chrono::milliseconds((long)(delta_sec * 1000));
        }
        return t;
    }

    std::vector<Satellite::PassEvent> PassPredictor::predict(Satellite& sat, const TimePoint& start, int search_window_mins) {
        std::vector<Satellite::PassEvent> results;
        TimePoint t = start;
        TimePoint end = start + std::chrono::minutes(search_window_mins);
        auto step = std::chrono::minutes(2);          // coarse scan resolution
        double prev_el = getElevation(sat, t);

        while (t < end) {
            TimePoint next_t = t + step;
            double next_el = getElevation(sat, next_t);

            // Sign change in elevation => the satellite crossed the horizon.
            if ((prev_el < 0 && next_el >= 0) || (prev_el >= 0 && next_el < 0)) {
                TimePoint crossing = solveNewton(sat, t + step/2);
                // Slope at the crossing distinguishes a rise (AOS) from a set (LOS).
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
