// tle_to_state.cpp -- Implementation of the TLE → TEME state helper.
//
// The only frame conversion happening here is DateTime → std::chrono, which is
// a pure time-scale bookkeeping issue, not a coordinate rotation.

#include "od/tle_to_state.hpp"

#include <SGP4.h>
#include <stdexcept>
#include <string>
#include <ctime>

namespace ve::od {

namespace {

// libsgp4::DateTime carries a "Ticks" count of 100-ns intervals since
// 0001-01-01 00:00 (proleptic Gregorian). Convert to Julian Date and to a
// UTC std::chrono::system_clock time point.
//
// Standard reference: J2000.0 = 2000-01-01 12:00 UTC = JD 2451545.0.
// The Unix epoch (1970-01-01 00:00 UTC) = JD 2440587.5. libsgp4's DateTime
// exposes ToJulian() for us; we only need to construct a matching TimePoint.
ve::TimePoint datetime_to_timepoint(const libsgp4::DateTime& dt) {
    // libsgp4::DateTime lacks a direct .to_time_t(); use its calendar accessors.
    std::tm gm{};
    gm.tm_year = dt.Year()   - 1900;
    gm.tm_mon  = dt.Month()  - 1;
    gm.tm_mday = dt.Day();
    gm.tm_hour = dt.Hour();
    gm.tm_min  = dt.Minute();
    gm.tm_sec  = dt.Second();
    // timegm() interprets the struct as UTC (POSIX extension; available on
    // glibc and BSDs). This is important — mktime() would apply TZ.
    time_t tt = timegm(&gm);
    if (tt == static_cast<time_t>(-1)) {
        throw std::runtime_error(
            "tle_to_state: timegm() failed to convert TLE epoch (broken calendar)");
    }
    auto tp = ve::Clock::from_time_t(tt);
    // Add sub-second fractional part.
    int micros = dt.Microsecond();
    tp += std::chrono::microseconds(micros);
    return tp;
}

} // namespace

TleEpochState teme_state_at_epoch(const libsgp4::Tle& tle) {
    libsgp4::SGP4 sgp4(tle);
    const libsgp4::DateTime epoch = tle.Epoch();
    // SGP4::FindPosition evaluated AT the element-set epoch returns the
    // osculating state in TEME (SGP4's native frame). This is exactly what
    // NumericalPropagator's constructor does; we duplicate the call so this
    // helper is usable without constructing a propagator.
    const libsgp4::Eci eci = sgp4.FindPosition(epoch);

    TleEpochState out;
    out.t_epoch_utc = datetime_to_timepoint(epoch);
    out.jd_epoch    = epoch.ToJulian();
    out.r_km        = {eci.Position().x, eci.Position().y, eci.Position().z};
    out.v_km_s      = {eci.Velocity().x, eci.Velocity().y, eci.Velocity().z};
    return out;
}

TleEpochState teme_state_at_epoch(const std::string& tle_name,
                                  const std::string& tle_line_1,
                                  const std::string& tle_line_2) {
    try {
        libsgp4::Tle tle(tle_name, tle_line_1, tle_line_2);
        return teme_state_at_epoch(tle);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string(
            "tle_to_state: libsgp4 rejected the TLE: ") + e.what());
    }
}

} // namespace ve::od
