// tle_to_state.hpp -- TLE → TEME (r, v) state converter for the OD subsystem.
//
// This is a THIN wrapper around libsgp4 that produces the TEME state at the
// TLE epoch. The OD subsystem operates in TEME end-to-end (see
// docs/orbit_determination.md §6); no frame conversion is performed here.
//
// If the caller needs the state at a later time (e.g. AOS), they should
// construct a ve::NumericalPropagator with the same TLE and call
// propagate(t_aos). Doing it in two steps keeps this helper pure (just a
// coordinate lookup at epoch, no numerical integration).

#pragma once

#include "types.hpp"
#include <Tle.h>

#include <utility>
#include <string>

namespace ve::od {

// Returned by teme_state_at_epoch(). All quantities in TEME frame.
struct TleEpochState {
    ve::TimePoint t_epoch_utc;   // TLE epoch as UTC TimePoint
    double        jd_epoch;      // TLE epoch as Julian Date (UTC)
    ve::Vector3   r_km;          // position at TLE epoch, TEME, km
    ve::Vector3   v_km_s;        // velocity at TLE epoch, TEME, km/s
};

// Evaluate SGP4 exactly at the TLE epoch (no propagation) and return the
// TEME osculating state. This is the standard "seed the numerical
// propagator from the TLE" step and matches how NumericalPropagator's own
// constructor initialises itself.
//
// Throws std::runtime_error if libsgp4 cannot evaluate the TLE (bad checksum,
// decayed elements, etc).
TleEpochState teme_state_at_epoch(const libsgp4::Tle& tle);

// Convenience: parse a TLE from its three text lines (name / line 1 / line 2)
// and return the epoch state. The name string is not used numerically; it is
// only carried through libsgp4::Tle's constructor.
TleEpochState teme_state_at_epoch(const std::string& tle_name,
                                  const std::string& tle_line_1,
                                  const std::string& tle_line_2);

} // namespace ve::od
