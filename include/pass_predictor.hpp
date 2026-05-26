// pass_predictor.hpp - AOS/LOS pass prediction.
//
// Predicts horizon crossings (Acquisition/Loss Of Signal) for a satellite over
// a search window: a coarse time scan brackets each elevation zero-crossing,
// then Newton's method refines it to the exact crossing time. Results are
// returned as PassEvent records (time + is_aos flag). Works against whatever
// propagator the Satellite uses, so it is correct for both SGP4 and HPOP.
#pragma once
#include "satellite.hpp"
#include "observer.hpp"

namespace ve {
    class PassPredictor {
    public:
        PassPredictor(const Observer& obs);
        std::vector<Satellite::PassEvent> predict(Satellite& sat, const TimePoint& start, int search_window_mins = 1440);

    private:
        Observer observer_;
        double getElevation(const Satellite& sat, const TimePoint& t);
        TimePoint solveNewton(const Satellite& sat, TimePoint initial_guess);
    };
}
