#pragma once
#include <ncurses.h>
#include <vector>
#include <string>
#include <sstream>
#include <mutex>
#include "satellite.hpp"
#include "observer.hpp"
#include "visibility.hpp"

namespace ve {
    struct DisplayRow {
        std::string name;
        double az;
        double el;
        double range;
        double range_rate; 
        double lat;
        double lon;
        double apogee; 
        VisibilityCalculator::State state;
        int norad_id;
        std::string next_event; 
    };
    class Display {
    public:
        enum class InputResult { NONE, QUIT_NO_SAVE, SAVE_AND_QUIT, BREAK_LOOP };
        Display();
        ~Display();
        void update(const std::vector<DisplayRow>& rows, const Observer& obs, const TimePoint& t, int total_tracked, int filter_kept, bool show_all_rf, double min_el);
        InputResult handleInput();
        
        void setBlocking(bool blocking);
        
        // THREAD SAFE GETTER - FIX: REMOVED INLINE BODY FROM HEADER
        std::string getLastFrame() const; 

    private:
        enum class InputMode { NORMAL, CONFIRM_QUIT };
        InputMode input_mode_;
        void initColors();
        void drawHeader(const Observer& obs, const TimePoint& t, int visible, int total, int kept);
        void drawFooter();
        void drawScrollbar(int total_rows, int visible_rows);
        int scroll_offset_;
        std::string last_frame_buffer_;
        mutable std::mutex frame_mutex_; 
        int last_key_debug_; 
    };
}
