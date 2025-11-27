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
