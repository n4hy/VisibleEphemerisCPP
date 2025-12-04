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
        }
    }

    std::string Display::getLastFrame() const {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return last_frame_buffer_;
    }
    
    void Display::setBlocking(bool blocking) {
        timeout(blocking ? 100 : 0);
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
    
    void Display::update(const std::vector<DisplayRow>& rows, const Observer& obs, const TimePoint& t, int total_tracked, int filter_kept, bool show_all_rf, double min_el) {
        std::stringstream ss; 
        
        drawHeader(obs, t, rows.size(), total_tracked, filter_kept);
        
        std::time_t tt = Clock::to_time_t(t);
        ss << "VISIBLE EPHEMERIS v12.65-CODE-ONLY\n";
        ss << std::put_time(std::gmtime(&tt), "%Y-%m-%d %H:%M:%S UTC") << "\n";
        auto loc = obs.getLocation();
        ss << "OBS: " << loc.lat_deg << ", " << loc.lon_deg << " | SHOWN: " << rows.size() << "\n\n";

        int start_y = 5;
        int available_lines = LINES - start_y - 1; 
        int max_offset = (int)rows.size() - available_lines;
        if (max_offset < 0) max_offset = 0;
        if (scroll_offset_ > max_offset) scroll_offset_ = max_offset;

        const char* hdr_fmt = "%-15s %8s %8s %10s %8s %-5s %-12s";
        if(input_mode_ != InputMode::CONFIRM_QUIT) {
            mvprintw(3, 0, hdr_fmt, "NAME", "AZ", "EL", "RANGE", "RR(km/s)", "VIS", "NEXT EVENT");
            clrtoeol(); 
            mvprintw(4, 0, "-------------------------------------------------------------------------");
            clrtoeol(); 
        }
        
        char buf[256];
        snprintf(buf, sizeof(buf), hdr_fmt, "NAME", "AZ", "EL", "RANGE", "RR(km/s)", "VIS", "NEXT EVENT");
        ss << buf << "\n-------------------------------------------------------------------------\n";

        // --- TEXT BUFFER GENERATION (SORTED BY VISIBILITY THEN NAME) ---
        std::vector<DisplayRow> text_rows = rows;
        std::sort(text_rows.begin(), text_rows.end(), [](const DisplayRow& a, const DisplayRow& b) {
            bool a_vis = (a.el >= 0.0);
            bool b_vis = (b.el >= 0.0);
            if (a_vis != b_vis) return a_vis > b_vis;
            return a.name < b.name;
        });

        for (const auto& r : text_rows) {
            std::string state_str = "---";
            if (r.state == VisibilityCalculator::State::VISIBLE) state_str = "VIS";
            else if (r.state == VisibilityCalculator::State::DAYLIGHT) state_str = "DAY";
            else if (r.state == VisibilityCalculator::State::ECLIPSED) state_str = "ECL";
            if (r.el < 0) state_str = "HOR";

            const char* row_fmt = "%-15s %8.1f %8.1f %10.1f %8.3f %-5s %-12s";
            snprintf(buf, sizeof(buf), row_fmt, 
                     r.name.substr(0,14).c_str(), r.az, r.el, r.range, r.range_rate, 
                     state_str.c_str(), r.next_event.c_str());
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

                if (r.el < 0) {
                    color = 4; // RED
                    state_str = "HOR";
                } else {
                    if (r.state == VisibilityCalculator::State::VISIBLE) { state_str = "VIS"; color=1; }
                    else if (r.state == VisibilityCalculator::State::DAYLIGHT) { state_str = "DAY"; color=2; }
                    else if (r.state == VisibilityCalculator::State::ECLIPSED) { state_str = "ECL"; color=3; }
                }

                if (std::abs(r.el - min_el) < 1.0) {
                    if (flash_state) color = 8; 
                    else color = 4;
                }

                const char* row_fmt = "%-15s %8.1f %8.1f %10.1f %8.3f %-5s %-12s";
                
                attron(COLOR_PAIR(color));
                mvprintw(start_y + i, 0, row_fmt, 
                         r.name.substr(0,14).c_str(), r.az, r.el, r.range, r.range_rate, 
                         state_str.c_str(), r.next_event.c_str());
                attroff(COLOR_PAIR(color));
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

    void Display::drawHeader(const Observer& obs, const TimePoint& t, int visible, int total, int kept) {
        std::time_t tt = Clock::to_time_t(t);
        std::tm* gmt = std::gmtime(&tt);
        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", gmt);
        
        attron(COLOR_PAIR(5));
        move(0,0);
        printw("VISIBLE EPHEMERIS v12.65-CODE-ONLY - CONF: config.yaml");
        for(int k=getcurx(stdscr); k<COLS-30; k++) addch(' '); 
        mvprintw(0, COLS-30, "%s", time_buf);
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
