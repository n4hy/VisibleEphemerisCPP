#pragma once
#include <ncurses.h>
#include <vector>
#include <string>
#include <iomanip>
#include <sstream>
#include "satellite.hpp"
#include "observer.hpp"
#include "visibility.hpp"

namespace ve {
    struct DisplayRow {
        std::string name;
        double az;
        double el;
        double range;
        // Added Lat/Lon for map
        double lat;
        double lon;
        double apogee; // Added Apogee
        VisibilityCalculator::State state;
        int norad_id;
    };
    class Display {
    public:
        Display();
        ~Display();
        void update(const std::vector<DisplayRow>& rows, const Observer& obs, const TimePoint& t, int total_tracked, int display_limit, int filter_kept);
        bool shouldQuit();
    private:
        void initColors();
        void drawHeader(const Observer& obs, const TimePoint& t, int visible, int total, int kept);
        void drawFooter();
    };
}
