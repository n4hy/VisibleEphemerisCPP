#include "display.hpp"
#include <algorithm>

namespace ve {
    Display::Display() {
        initscr();
        cbreak();
        noecho();
        nodelay(stdscr, TRUE);
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
        }
    }
    bool Display::shouldQuit() { return (getch() == 'q'); }
    void Display::update(const std::vector<DisplayRow>& rows, const Observer& obs, const TimePoint& t, int total_tracked, int display_limit, int filter_kept) {
        clear();
        drawHeader(obs, t, rows.size(), total_tracked, filter_kept);
        int row_idx = 4;
        mvprintw(row_idx++, 0, "%-15s %-8s %-8s %-10s %-5s", "NAME", "AZ", "EL", "RANGE", "VIS");
        mvprintw(row_idx++, 0, "--------------------------------------------------");
        
        int count = 0;
        for (const auto& r : rows) {
            if (count++ >= display_limit) break;
            
            int color = 3;
            std::string state_str = "---";
            if (r.state == VisibilityCalculator::State::VISIBLE) { color = 1; state_str = "VIS"; }
            else if (r.state == VisibilityCalculator::State::DAYLIGHT) { color = 2; state_str = "DAY"; }
            else if (r.state == VisibilityCalculator::State::ECLIPSED) { color = 3; state_str = "ECL"; }
            attron(COLOR_PAIR(color));
            mvprintw(row_idx, 0, "%-15s %8.1f %8.1f %10.1f %-5s", 
                     r.name.substr(0,14).c_str(), r.az, r.el, r.range, state_str.c_str());
            attroff(COLOR_PAIR(color));
            row_idx++;
            if (row_idx >= LINES - 2) break; 
        }
        drawFooter();
        refresh();
    }
    void Display::drawHeader(const Observer& obs, const TimePoint& t, int visible, int total, int kept) {
        std::time_t tt = Clock::to_time_t(t);
        std::tm* gmt = std::gmtime(&tt);
        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", gmt);
        attron(COLOR_PAIR(5));
        for(int i=0; i<COLS; i++) mvaddch(0, i, ' ');
        mvprintw(0, 1, "VISIBLE EPHEMERIS v12.0 - SERVER: http://<IP>:8080");
        mvprintw(0, COLS-30, "%s", time_buf);
        attroff(COLOR_PAIR(5));
        auto loc = obs.getLocation();
        mvprintw(1, 1, "OBSERVER: %.4f, %.4f  |  TRACKED: %d  |  FILTERED: %d  |  DISPLAY: %d", 
                 loc.lat_deg, loc.lon_deg, total, kept, visible);
    }
    void Display::drawFooter() {
        attron(COLOR_PAIR(5));
        for(int i=0; i<COLS; i++) mvaddch(LINES-1, i, ' ');
        mvprintw(LINES-1, 1, "Controls: [q] Quit");
        attroff(COLOR_PAIR(5));
    }
}
