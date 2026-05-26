// display.cpp - ncurses terminal UI implementation.
// The constructor initializes curses (raw mode, no echo, hidden cursor, 100 ms
// input timeout) and a colour palette keyed to visibility/flare state. update()
// draws the header, the elevation-sorted satellite table (with a scrollbar and
// below-minimum-elevation dimming) and footer, and stores a plain-text snapshot
// of the frame (frame_mutex_) for the text-mirror server. handleInput() drives
// scrolling and a confirm-on-quit flow.
#include "display.hpp"
#include <algorithm>
#include <iomanip>

namespace ve {
    Display::Display() : scroll_offset_(0), input_mode_(InputMode::NORMAL), last_frame_buffer_("Waiting for data..."), last_key_debug_(0) {
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE); 
        timeout(100); 
        curs_set(0);
        initColors();
    }
    Display::~Display() { endwin(); }
    
    void Display::initColors() {
        if (has_colors()) {
            start_color();
            init_pair(1, COLOR_GREEN, COLOR_BLACK);
            init_pair(2, COLOR_YELLOW, COLOR_BLACK);
            init_pair(3, COLOR_CYAN, COLOR_BLACK);
            init_pair(4, COLOR_RED, COLOR_BLACK);
            init_pair(5, COLOR_WHITE, COLOR_BLUE);
            init_pair(6, COLOR_BLACK, COLOR_WHITE);
            init_pair(7, COLOR_WHITE, COLOR_RED);
            init_pair(8, COLOR_RED, COLOR_WHITE); // Flash: Red/White
            init_pair(9, COLOR_WHITE, COLOR_BLACK); // Grey (dim white) for below min_el
        }
    }

    std::string Display::getLastFrame() const {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return last_frame_buffer_;
    }
    
    void Display::setBlocking(bool blocking, int timeout_ms) {
        timeout(blocking ? timeout_ms : 0);
    }
    
    Display::InputResult Display::handleInput() {
        int ch = getch();
        if (ch == ERR) return InputResult::NONE;
        last_key_debug_ = ch;

        if (input_mode_ == InputMode::CONFIRM_QUIT) {
            if (ch == 'y' || ch == 'Y') return InputResult::SAVE_AND_QUIT;
            if (ch == 'n' || ch == 'N') return InputResult::QUIT_NO_SAVE;
            if (ch == 27) { input_mode_ = InputMode::NORMAL; return InputResult::NONE; }
            return InputResult::BREAK_LOOP; // Force refresh on any input
        }
        if (ch == 'q' || ch == 'Q') { 
            input_mode_ = InputMode::CONFIRM_QUIT; 
            return InputResult::BREAK_LOOP; // KEY FIX: ABORT MATH LOOP NOW
        }
        if (ch == KEY_UP) { scroll_offset_--; if (scroll_offset_ < 0) scroll_offset_ = 0; }
        else if (ch == KEY_DOWN) scroll_offset_++;
        else if (ch == KEY_PPAGE) { scroll_offset_ -= 10; if (scroll_offset_ < 0) scroll_offset_ = 0; }
        else if (ch == KEY_NPAGE) scroll_offset_ += 10;
        
        return InputResult::NONE;
    }
    
    void Display::update(const std::vector<DisplayRow>& rows, const Observer& obs, const TimePoint& t, int total_tracked, int filter_kept, bool show_all_rf, double min_el, const std::string& time_str) {
        std::stringstream ss; 
        
        drawHeader(obs, rows.size(), total_tracked, filter_kept, time_str);
        
        std::time_t tt = Clock::to_time_t(t);
        ss << "VISIBLE EPHEMERIS v12.65-CODE-ONLY\n";
        ss << time_str << "\n";
        auto loc = obs.getLocation();
        ss << "OBS: " << loc.lat_deg << ", " << loc.lon_deg << " | SHOWN: " << rows.size() << "\n\n";

        int start_y = 5;
        int available_lines = LINES - start_y - 1; 
        int max_offset = (int)rows.size() - available_lines;
        if (max_offset < 0) max_offset = 0;
        if (scroll_offset_ > max_offset) scroll_offset_ = max_offset;

        const char* hdr_fmt = "%-15s %8s %8s %10s %8s %-5s %-12s";
        const char* text_hdr_fmt = "%-15s %8s %8s %10s %8s %-5s %-12s %8s %10s %10s %10s %6s";
        if(input_mode_ != InputMode::CONFIRM_QUIT) {
            mvprintw(3, 0, hdr_fmt, "NAME", "AZ", "EL", "RANGE", "RR(km/s)", "VIS", "NEXT EVENT");
            clrtoeol();
            mvprintw(4, 0, "-------------------------------------------------------------------------");
            clrtoeol();
        }

        char buf[512];
        snprintf(buf, sizeof(buf), text_hdr_fmt, "NAME", "AZ", "EL", "RANGE", "RR(km/s)", "VIS", "NEXT EVENT", "NORAD", "LAT", "LON", "APOGEE", "FLARE");
        ss << buf << "\n----------------------------------------------------------------------------------------------------------------------\n";

        // --- TEXT BUFFER GENERATION (SORTED BY EL DESCENDING) ---
        std::vector<DisplayRow> text_rows = rows;
        std::sort(text_rows.begin(), text_rows.end(), [](const DisplayRow& a, const DisplayRow& b) {
            return a.el > b.el;
        });

        for (const auto& r : text_rows) {
            std::string state_str = "---";
            if (r.state == VisibilityCalculator::State::VISIBLE) state_str = "VIS";
            else if (r.state == VisibilityCalculator::State::DAYLIGHT) state_str = "DAY";
            else if (r.state == VisibilityCalculator::State::ECLIPSED) state_str = "ECL";
            if (r.el < 0) state_str = "HOR";

            std::string d_name = r.name.substr(0,14);
            if (r.flare_status > 0) d_name += " F";

            const char* text_row_fmt = "%-15s %8.1f %8.1f %10.1f %8.3f %-5s %-12s %8d %10.4f %10.4f %10.1f %6d";
            snprintf(buf, sizeof(buf), text_row_fmt,
                     d_name.c_str(), r.az, r.el, r.range, r.range_rate,
                     state_str.c_str(), r.next_event.c_str(),
                     r.norad_id, r.lat, r.lon, r.apogee, r.flare_status);
            ss << buf << "\n";
        }

        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            last_frame_buffer_ = ss.str();
        }

        if (input_mode_ == InputMode::CONFIRM_QUIT) {
            attron(COLOR_PAIR(7));
            mvprintw(LINES/2, COLS/2 - 20, " SAVE CONFIGURATION? (Y/N/ESC) ");
            attroff(COLOR_PAIR(7));
        } else {
             if (rows.empty()) {
                mvprintw(start_y, 0, "NO SATELLITES FOUND. CHECK FILTERS.");
                clrtoeol();
            }

            bool flash_state = (tt % 2 == 0);

            for (int i = 0; i < available_lines; ++i) {
                int data_idx = scroll_offset_ + i;
                if (data_idx >= rows.size()) {
                    move(start_y + i, 0); clrtoeol(); continue;
                }
                const auto& r = rows[data_idx];
                int color = 3;
                std::string state_str = "---";
                bool use_dim = false;

                if (show_all_rf) {
                    // RADIO MODE (visible_only=false): New color scheme
                    // Yellow: above min_el AND VISIBLE
                    // Green: above min_el AND NOT VISIBLE (DAYLIGHT/ECLIPSED)
                    // Grey (dim): below min_el or below horizon
                    if (r.el < 0) {
                        color = 9; use_dim = true; // Grey for below horizon
                        state_str = "HOR";
                    } else if (r.el < min_el) {
                        color = 9; use_dim = true; // Grey for below min_el
                        if (r.state == VisibilityCalculator::State::VISIBLE) state_str = "VIS";
                        else if (r.state == VisibilityCalculator::State::DAYLIGHT) state_str = "DAY";
                        else if (r.state == VisibilityCalculator::State::ECLIPSED) state_str = "ECL";
                    } else {
                        // Above min_el
                        if (r.state == VisibilityCalculator::State::VISIBLE) {
                            state_str = "VIS"; color = 2; // Yellow
                        } else if (r.state == VisibilityCalculator::State::DAYLIGHT) {
                            state_str = "DAY"; color = 1; // Green
                        } else if (r.state == VisibilityCalculator::State::ECLIPSED) {
                            state_str = "ECL"; color = 1; // Green
                        }
                    }
                } else {
                    // OPTICAL MODE (visible_only=true): Original color scheme
                    if (r.el < 0) {
                        color = 4; // RED
                        state_str = "HOR";
                    } else {
                        if (r.state == VisibilityCalculator::State::VISIBLE) { state_str = "VIS"; color=1; }
                        else if (r.state == VisibilityCalculator::State::DAYLIGHT) { state_str = "DAY"; color=2; }
                        else if (r.state == VisibilityCalculator::State::ECLIPSED) { state_str = "ECL"; color=3; }
                    }
                }

                std::string d_name = r.name.substr(0,14);

                // FLARE LOGIC
                if (r.flare_status > 0) {
                    d_name += " F";
                    use_dim = false; // Flares override dim
                    if (r.flare_status == 2) {
                        // HIT: Fast Flash
                        long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
                        if ((ms / 200) % 2 == 0) color = 4; // RED
                        else color = 3; // CYAN
                    } else {
                        // NEAR: Slow Flash
                        if (flash_state) color = 4; // RED
                    }
                } else if (!show_all_rf && std::abs(r.el - min_el) < 1.0) {
                    // HORIZON FLASH (only in optical mode)
                    if (flash_state) color = 8;
                    else color = 4;
                }

                const char* row_fmt = "%-15s %8.1f %8.1f %10.1f %8.3f %-5s %-12s";

                if (use_dim) attron(A_DIM);
                attron(COLOR_PAIR(color));
                mvprintw(start_y + i, 0, row_fmt,
                         d_name.c_str(), r.az, r.el, r.range, r.range_rate,
                         state_str.c_str(), r.next_event.c_str());
                attroff(COLOR_PAIR(color));
                if (use_dim) attroff(A_DIM);
                clrtoeol(); 
            }
            
            clrtobot(); 
            drawScrollbar(rows.size(), available_lines);
        }
        
        drawFooter();
        refresh();
    }

    void Display::drawScrollbar(int total_rows, int visible_rows) {
        if (total_rows <= visible_rows) return;
        int start_y = 5;
        int bar_height = visible_rows;
        float ratio = (float)visible_rows / total_rows;
        int slider_size = (int)(bar_height * ratio);
        if (slider_size < 1) slider_size = 1;
        float pos_ratio = (float)scroll_offset_ / (total_rows - visible_rows);
        int slider_pos = (int)((bar_height - slider_size) * pos_ratio);
        for(int i=0; i<bar_height; ++i) mvaddch(start_y + i, COLS-1, '|');
        attron(COLOR_PAIR(6));
        for(int i=0; i<slider_size; ++i) mvaddch(start_y + slider_pos + i, COLS-1, ' ');
        attroff(COLOR_PAIR(6));
    }

    void Display::drawHeader(const Observer& obs, int visible, int total, int kept, const std::string& time_str) {
        attron(COLOR_PAIR(5));
        move(0,0);
        printw("VISIBLE EPHEMERIS v12.65-CODE-ONLY - CONF: config.yaml");
        for(int k=getcurx(stdscr); k<COLS-30; k++) addch(' '); 
        mvprintw(0, COLS-30, "%s", time_str.c_str());
        attroff(COLOR_PAIR(5));
        
        auto loc = obs.getLocation();
        mvprintw(1, 1, "OBSERVER: %.4f, %.4f  |  TRACKED: %d  |  SHOWN: %d", 
                 loc.lat_deg, loc.lon_deg, total, visible);
        clrtoeol();
    }
    void Display::drawFooter() {
        attron(COLOR_PAIR(5));
        move(LINES-1, 0);
        printw("Controls: [UP/DOWN] Scroll  [q] Quit  [LastKey: %d]", last_key_debug_);
        clrtoeol();
        attroff(COLOR_PAIR(5));
    }
}
