// pybind11 bindings exposing the High-Precision Orbit Propagator (HPOP) to
// Python. Build with -DBUILD_PYTHON_BINDINGS=ON (see CMakeLists.txt); the
// resulting module is `ve_hpop`.
//
//   import ve_hpop
//   p = ve_hpop.Propagator(name, line1, line2, degree=10)
//   r, v = p.propagate_jd(jd)            # ECI (TEME) km, km/s
//   lat, lon, alt = p.geodetic_jd(jd)    # WGS84 degrees, km
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/chrono.h>

#include "numerical_propagator.hpp"
#include "force_model.hpp"
#include "ephemeris.hpp"
#include "egm96_coeffs.hpp"

#include <Tle.h>
#include <Eci.h>
#include <Vector.h>
#include <CoordGeodetic.h>

#include <array>
#include <memory>
#include <string>
#include <utility>

namespace py = pybind11;
using namespace ve;

namespace {
    using Vec3 = std::array<double, 3>;
    using StatePair = std::pair<Vec3, Vec3>;

    Vec3 toArr(const Vector3& v) { return {v.x, v.y, v.z}; }
}

// Thin Python-facing wrapper: owns the numerical propagator plus the metadata
// needed for time/geodetic conversions.
class PyPropagator {
public:
    PyPropagator(const std::string& name, const std::string& line1, const std::string& line2,
                 int degree, int order, bool drag, bool srp, bool thirdbody,
                 double mass_kg, double drag_area_m2, double srp_area_m2,
                 double Cd, double Cr, double rtol, double atol)
        : name_(name) {
        libsgp4::Tle tle(name, line1, line2);
        norad_ = static_cast<int>(tle.NoradNumber());
        bstar_ = tle.BStar();
        epoch_ = tle.Epoch();

        ForceParams fp;
        fp.grav_degree = degree;
        fp.grav_order  = (order < 0) ? degree : order;
        fp.use_drag = drag;
        fp.use_srp = srp;
        fp.use_sun = fp.use_moon = thirdbody;
        fp.mass_kg = mass_kg;
        fp.drag_area_m2 = drag_area_m2;
        fp.srp_area_m2 = srp_area_m2;
        fp.Cd = Cd;
        fp.Cr = Cr;
        fp.bstar = bstar_;

        IntegratorParams ip;
        if (rtol > 0) ip.rtol = rtol;
        if (atol > 0) ip.atol = atol;

        prop_ = std::make_unique<NumericalPropagator>(tle, fp, ip);
    }

    bool valid() const { return prop_ && prop_->valid(); }
    double epoch_jd() const { return prop_->epochJulian(); }
    int norad_id() const { return norad_; }
    const std::string& name() const { return name_; }
    double bstar() const { return bstar_; }
    double cd_area_over_m() const { return prop_->forceModel().cdAoverM(); }
    double cr_area_over_m() const { return prop_->forceModel().crAoverM(); }
    bool drag_enabled() const { return prop_->forceModel().dragEnabled(); }
    Vec3 epoch_position() const { return toArr(prop_->epochPosition()); }
    Vec3 epoch_velocity() const { return toArr(prop_->epochVelocity()); }

    StatePair state_at_seconds(double t_sec) {
        auto sv = prop_->stateAtSeconds(t_sec);
        return {toArr(sv.first), toArr(sv.second)};
    }
    StatePair propagate_jd(double jd) {
        return state_at_seconds((jd - prop_->epochJulian()) * 86400.0);
    }
    StatePair propagate_unix(double unix_seconds) {
        return propagate_jd(2440587.5 + unix_seconds / 86400.0);
    }
    StatePair propagate_datetime(const TimePoint& t) {
        auto sv = prop_->propagate(t);
        return {toArr(sv.first), toArr(sv.second)};
    }

    Vec3 geodetic_at_seconds(double t_sec) {
        auto sv = prop_->stateAtSeconds(t_sec);
        libsgp4::DateTime dt = epoch_.AddSeconds(t_sec);
        libsgp4::Eci eci(dt, libsgp4::Vector(sv.first.x, sv.first.y, sv.first.z));
        libsgp4::CoordGeodetic geo = eci.ToGeodetic();
        return {geo.latitude * RAD2DEG, geo.longitude * RAD2DEG, geo.altitude};
    }
    Vec3 geodetic_jd(double jd) {
        return geodetic_at_seconds((jd - prop_->epochJulian()) * 86400.0);
    }

private:
    std::string name_;
    int norad_ = 0;
    double bstar_ = 0.0;
    libsgp4::DateTime epoch_;
    std::unique_ptr<NumericalPropagator> prop_;
};

PYBIND11_MODULE(ve_hpop, m) {
    m.doc() = "High-Precision Orbit Propagator (EGM96 gravity + Sun/Moon + drag + SRP, "
              "adaptive Fehlberg RK7(8)). Seeded from the SGP4 state vector at TLE epoch.";

    py::class_<PyPropagator>(m, "Propagator")
        .def(py::init<const std::string&, const std::string&, const std::string&,
                      int, int, bool, bool, bool, double, double, double,
                      double, double, double, double>(),
             py::arg("name"), py::arg("line1"), py::arg("line2"),
             py::arg("degree") = 10, py::arg("order") = -1,
             py::arg("drag") = true, py::arg("srp") = true, py::arg("thirdbody") = true,
             py::arg("mass_kg") = 0.0, py::arg("drag_area_m2") = 0.0, py::arg("srp_area_m2") = 0.0,
             py::arg("Cd") = 2.2, py::arg("Cr") = 1.3,
             py::arg("rtol") = 0.0, py::arg("atol") = 0.0,
             "Construct from a TLE. Geopotential degree/order default to 10 (up to 20);\n"
             "spacecraft mass/area are optional (the TLE B* term seeds the drag/SRP area-to-mass).")
        .def("propagate_jd", &PyPropagator::propagate_jd, py::arg("jd"),
             "Propagate to a UTC Julian Date; returns (position_km[3], velocity_km_s[3]) in ECI/TEME.")
        .def("propagate_unix", &PyPropagator::propagate_unix, py::arg("unix_seconds"),
             "Propagate to Unix epoch seconds (UTC); returns (position_km[3], velocity_km_s[3]).")
        .def("propagate", &PyPropagator::propagate_datetime, py::arg("when"),
             "Propagate to a Python datetime (UTC); returns (position_km[3], velocity_km_s[3]).")
        .def("state_at_seconds", &PyPropagator::state_at_seconds, py::arg("seconds_since_epoch"),
             "State a number of seconds after the TLE epoch; returns (pos_km[3], vel_km_s[3]).")
        .def("geodetic_jd", &PyPropagator::geodetic_jd, py::arg("jd"),
             "Sub-satellite WGS84 geodetic at a UTC Julian Date; returns (lat_deg, lon_deg, alt_km).")
        .def("geodetic_at_seconds", &PyPropagator::geodetic_at_seconds, py::arg("seconds_since_epoch"),
             "Sub-satellite WGS84 geodetic; returns (lat_deg, lon_deg, alt_km).")
        .def_property_readonly("valid", &PyPropagator::valid)
        .def_property_readonly("name", &PyPropagator::name)
        .def_property_readonly("norad_id", &PyPropagator::norad_id)
        .def_property_readonly("bstar", &PyPropagator::bstar)
        .def_property_readonly("epoch_jd", &PyPropagator::epoch_jd)
        .def_property_readonly("epoch_position", &PyPropagator::epoch_position)
        .def_property_readonly("epoch_velocity", &PyPropagator::epoch_velocity)
        .def_property_readonly("cd_area_over_m", &PyPropagator::cd_area_over_m,
                               "Effective Cd*A/m used by the drag model (m^2/kg).")
        .def_property_readonly("cr_area_over_m", &PyPropagator::cr_area_over_m,
                               "Effective Cr*A/m used by the SRP model (m^2/kg).")
        .def_property_readonly("drag_enabled", &PyPropagator::drag_enabled);

    // Free helpers mirroring the C++ models.
    m.def("sun_position_eci", [](double jd) { return toArr(sunPositionECI(jd)); }, py::arg("jd"),
          "Geocentric Sun position in ECI (km) at a Julian Date.");
    m.def("moon_position_eci", [](double jd) { return toArr(moonPositionECI(jd)); }, py::arg("jd"),
          "Geocentric Moon position in ECI (km) at a Julian Date.");
    m.def("atmosphere_density", &ForceModel::atmosphereDensity, py::arg("alt_km"),
          "Piecewise-exponential atmospheric density (kg/m^3) at geometric altitude (km).");
    m.def("gmst_rad", &gmstFromJD, py::arg("jd"),
          "Greenwich Mean Sidereal Time (radians) at a UTC Julian Date.");

    m.attr("GM_KM3_S2") = egm96::GM_KM3_S2;
    m.attr("R_REF_KM") = egm96::R_REF_KM;
    m.attr("MAX_DEGREE") = egm96::MAX_DEGREE;
    m.attr("EARTH_RADIUS_KM") = EARTH_RADIUS_KM;
    m.attr("__version__") = "1.0";
}
