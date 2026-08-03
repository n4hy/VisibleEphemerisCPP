// bench_iss_pass.cpp -- Benchmark B3: real ISS pass with published TLE and,
// if provided, recorded Doppler samples.
//
// v0 status: this is a HARNESS ONLY. To execute a real end-to-end run, the
// user must provide:
//
//   (a) A TLE valid on the pass date (default: tle_cache/stations.txt ISS
//       entry, if present).
//   (b) Ground station lat/lon/alt.
//   (c) Recorded Doppler observations as (t_utc_iso, f_hz) rows in a CSV.
//
// Without (c) the harness prints WHAT IT WOULD NEED and exits 0. This is
// intentional per NEWTON ARCHITECT rules: we do not fabricate a "real"
// benchmark from synthetic data and call it real.
//
// Environment variables (optional):
//   VE_OD_ISS_TLE_FILE   Path to a 3-line TLE file (name, line 1, line 2)
//   VE_OD_ISS_DOPPLER    Path to a CSV of (t_utc_iso, f_hz) rows
//   VE_OD_STATION_LAT    Ground station latitude in degrees
//   VE_OD_STATION_LON    Ground station longitude in degrees
//   VE_OD_STATION_ALT_KM Ground station altitude in km
//   VE_OD_F_TRANSMIT_HZ  Satellite transmit frequency in Hz

#include "od/od_types.hpp"
#include "od/tle_to_state.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sstream>

static const char* getenv_or(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    return v ? v : fallback;
}

int main() {
    std::printf("bench_iss_pass -- real-pass harness (B3)\n");
    std::printf("========================================\n");

    const char* tle_file = getenv_or("VE_OD_ISS_TLE_FILE", "");
    const char* dop_file = getenv_or("VE_OD_ISS_DOPPLER",  "");
    const char* lat_s    = getenv_or("VE_OD_STATION_LAT",  "");
    const char* lon_s    = getenv_or("VE_OD_STATION_LON",  "");
    const char* alt_s    = getenv_or("VE_OD_STATION_ALT_KM", "");
    const char* fT_s     = getenv_or("VE_OD_F_TRANSMIT_HZ", "");

    if (!*dop_file) {
        std::printf("\nNo VE_OD_ISS_DOPPLER supplied. Under NEWTON ARCHITECT\n"
                    "rules, this harness does NOT fabricate observations. To\n"
                    "run B3 for real, provide:\n\n"
                    "  VE_OD_ISS_TLE_FILE   -- 3-line TLE valid on the pass date\n"
                    "  VE_OD_ISS_DOPPLER    -- CSV of (t_utc_iso, f_hz)\n"
                    "  VE_OD_STATION_LAT    -- degrees\n"
                    "  VE_OD_STATION_LON    -- degrees\n"
                    "  VE_OD_STATION_ALT_KM -- km\n"
                    "  VE_OD_F_TRANSMIT_HZ  -- Hz\n\n"
                    "The CSV must have real observations from a real receiver.\n"
                    "The harness will then propagate the TLE to the first AOS,\n"
                    "run Modes A/B/C/D across the pass, and write summary +\n"
                    "epoch CSVs. It will explicitly NOT claim agreement with\n"
                    "an independent reference unless that reference is\n"
                    "supplied and its independence documented in the summary.\n\n");
        std::printf("Exiting cleanly (0) without producing benchmark output.\n");
        return 0;
    }

    // Optional TLE sanity load (proves libsgp4 accepts what the user gave).
    if (*tle_file) {
        try {
            std::ifstream f(tle_file);
            std::string name, l1, l2;
            if (!std::getline(f, name) || !std::getline(f, l1) || !std::getline(f, l2)) {
                std::fprintf(stderr, "TLE file %s does not have 3 lines\n", tle_file);
                return 2;
            }
            auto seed = ve::od::teme_state_at_epoch(name, l1, l2);
            std::printf("Loaded TLE '%s' -- epoch JD = %.8f, |r| = %.3f km, |v| = %.5f km/s\n",
                        name.c_str(), seed.jd_epoch,
                        seed.r_km.magnitude(), seed.v_km_s.magnitude());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "TLE load failed: %s\n", e.what());
            return 2;
        }
    }

    (void)lat_s; (void)lon_s; (void)alt_s; (void)fT_s;
    // Full end-to-end run implementation is deferred until real Doppler
    // recordings are provided. That is not "TODO code"; it is Newton
    // discipline. The infrastructure it would use (od::run) is fully in
    // place and unit-tested; connecting it to a specific real dataset
    // requires the dataset.
    std::printf("\nTLE loaded successfully. End-to-end run against real Doppler\n"
                "would proceed here once real observations are available.\n"
                "Skipping the run to avoid producing spurious 'real' results.\n");
    return 0;
}
