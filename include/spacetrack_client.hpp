// spacetrack_client.hpp - Space-Track.org historical TLE client.
//
// Fetches the element set that was current on a given past date via the
// gp_history endpoint (used when --time selects a date >24 h ago). See the
// class comment below for the credential-resolution order and fetch semantics.
#pragma once
#include <string>
#include <vector>
#include <ctime>

namespace ve {

    // Minimal Space-Track.org client for historical TLE retrieval (gp_history endpoint).
    // Credentials are resolved from (in order):
    //   1. environment variables SPACETRACK_USER and SPACETRACK_PASS
    //   2. file ~/.config/visible-ephemeris/spacetrack.ini ([spacetrack] username=, password=)
    class SpaceTrackClient {
    public:
        SpaceTrackClient();

        // True if credentials were found. If false, any historical fetch will fail.
        bool hasCredentials() const { return !username_.empty() && !password_.empty(); }

        // Human-readable hint used for error messages when hasCredentials() is false.
        static std::string credentialsHelpText();

        // Fetch gp_history for the given NORAD IDs at (or just before) target_date.
        // Writes 3LE (three-line element set) output to dest_path: one TLE per NORAD ID,
        // the latest whose epoch falls in [target_date - window_days, target_date]
        // (the window is one-sided — never an epoch after the target instant).
        // Returns true on success.
        bool fetchHistoricalTLEs(const std::vector<int>& norad_ids,
                                 std::time_t target_date,
                                 int window_days,
                                 const std::string& dest_path);

    private:
        std::string username_;
        std::string password_;

        void loadCredentials();
        static std::string urlEncode(const std::string& s);
    };
}
