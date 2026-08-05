// python_bindings_od.cpp -- pybind11 bindings for the orbit-determination
// subsystem.
//
// Scope (v0, deliberately narrow after debugging session):
//   * Value types with no Eigen members: Vector3, Geodetic, ForceParams
//   * ForceModel constructor
//   * TleEpochState + teme_state_at_epoch (TLE -> ECI/TEME seed at epoch)
//   * OrbitDiagnostics + diagnose_state (two-body a, e sanity check)
//
// NOT bound in v0:
//   * ve::od::run and everything downstream of the SRUKF (FilterConfig,
//     PassInput, PassResult, IterationConfig, Mode). Any binding of these
//     -- direct struct binding, def_property copy accessors, or a
//     functional wrapper that packs a PassInput internally -- reliably
//     segfaults inside NLF's SigmaPoints::generate_sigma_points_from_sqrt
//     when the module is dlopen'd. The failure reproduces with
//     EIGEN_MAX_ALIGN_BYTES=0 applied to both the module and ve_od_lib
//     so it is not a simple alignment mismatch. Root-causing is out of
//     scope for the session in which these bindings were introduced.
//   * predict_doppler_hz and doppler_jacobian as free functions -- they
//     take a fixed-size Eigen state vector as a positional argument and
//     the ndarray->Eigen conversion segfaults on the same code path.
//
// The Live-OD dashboard therefore drives its own Python-side EKF using
// the audited Doppler formulas mirrored in Python (see
// MolniyaTwelveHour.py for the reference implementation). This module
// provides only the TLE-seed and plausibility-check helpers that the
// dashboard needs. When the SRUKF binding issue is resolved, this file
// can be extended without breaking the current dashboard.
//
// Build: enable both -DBUILD_OD=ON and -DBUILD_PYTHON_BINDINGS=ON.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/chrono.h>

#include "od/od_types.hpp"
#include "od/tle_to_state.hpp"
#include "force_model.hpp"
#include "types.hpp"

#include <cstdio>
#include <string>

namespace py = pybind11;
using namespace ve;
using namespace ve::od;

PYBIND11_MODULE(ve_od, m) {
    m.doc() =
        "Narrow bindings for the VisibleEphemeris orbit-determination "
        "subsystem. Exposes TLE-seed and plausibility helpers only; the "
        "SRUKF+RTS driver is not currently bound (see the file header for "
        "the segfault trail).";

    // ----- Value types -------------------------------------------------

    py::class_<ve::Vector3>(m, "Vector3")
        .def(py::init([](double x, double y, double z) {
                 return ve::Vector3{x, y, z};
             }),
             py::arg("x") = 0.0, py::arg("y") = 0.0, py::arg("z") = 0.0)
        .def_readwrite("x", &ve::Vector3::x)
        .def_readwrite("y", &ve::Vector3::y)
        .def_readwrite("z", &ve::Vector3::z)
        .def("magnitude", &ve::Vector3::magnitude)
        .def("__repr__", [](const ve::Vector3& v) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "Vector3(%g, %g, %g)", v.x, v.y, v.z);
            return std::string(buf);
        });

    py::class_<ve::Geodetic>(m, "Geodetic")
        .def(py::init([](double lat_deg, double lon_deg, double alt_km) {
                 return ve::Geodetic{lat_deg, lon_deg, alt_km};
             }),
             py::arg("lat_deg") = 0.0,
             py::arg("lon_deg") = 0.0,
             py::arg("alt_km")  = 0.0)
        .def_readwrite("lat_deg", &ve::Geodetic::lat_deg)
        .def_readwrite("lon_deg", &ve::Geodetic::lon_deg)
        .def_readwrite("alt_km",  &ve::Geodetic::alt_km);

    py::class_<ve::ForceParams>(m, "ForceParams")
        .def(py::init<>())
        .def_readwrite("grav_degree", &ve::ForceParams::grav_degree)
        .def_readwrite("grav_order",  &ve::ForceParams::grav_order)
        .def_readwrite("use_sun",  &ve::ForceParams::use_sun)
        .def_readwrite("use_moon", &ve::ForceParams::use_moon)
        .def_readwrite("use_drag", &ve::ForceParams::use_drag)
        .def_readwrite("use_srp",  &ve::ForceParams::use_srp)
        .def_readwrite("mass_kg",      &ve::ForceParams::mass_kg)
        .def_readwrite("drag_area_m2", &ve::ForceParams::drag_area_m2)
        .def_readwrite("srp_area_m2",  &ve::ForceParams::srp_area_m2)
        .def_readwrite("Cd", &ve::ForceParams::Cd)
        .def_readwrite("Cr", &ve::ForceParams::Cr)
        .def_readwrite("bstar", &ve::ForceParams::bstar)
        .def_readwrite("use_iau2000_rotation", &ve::ForceParams::use_iau2000_rotation);

    py::class_<ve::ForceModel>(m, "ForceModel")
        .def(py::init<const ve::ForceParams&>(), py::arg("params"));

    // ----- TLE seed + orbit diagnostics --------------------------------

    py::class_<TleEpochState>(m, "TleEpochState")
        .def_readonly("t_epoch_utc", &TleEpochState::t_epoch_utc)
        .def_readonly("jd_epoch",    &TleEpochState::jd_epoch)
        .def_readonly("r_km",        &TleEpochState::r_km)
        .def_readonly("v_km_s",      &TleEpochState::v_km_s);

    m.def("teme_state_at_epoch",
          py::overload_cast<const std::string&, const std::string&, const std::string&>(
              &teme_state_at_epoch),
          py::arg("name"), py::arg("line1"), py::arg("line2"),
          "Evaluate SGP4 at the TLE epoch; returns TleEpochState in TEME.");

    py::class_<OrbitDiagnostics>(m, "OrbitDiagnostics")
        .def_readonly("semi_major_axis_km", &OrbitDiagnostics::semi_major_axis_km)
        .def_readonly("eccentricity",       &OrbitDiagnostics::eccentricity)
        .def_readonly("specific_energy_km2_s2", &OrbitDiagnostics::specific_energy_km2_s2)
        .def_readonly("is_plausible",       &OrbitDiagnostics::is_plausible)
        .def_property_readonly("reason", [](const OrbitDiagnostics& d) {
            return std::string(d.reason ? d.reason : "");
        });

    m.def("diagnose_state", &diagnose_state,
          py::arg("r"), py::arg("v"),
          "Two-body semi-major axis + eccentricity + plausibility flag "
          "for a claimed inertial-frame (r, v). Catches ECEF-in-ECI mistakes.");
}
