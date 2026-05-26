#pragma once
#include "types.hpp"
#include <vector>

// High-precision force model for Earth-orbiting satellites. Computes the total
// inertial acceleration (km/s^2) acting on a spacecraft from:
//   * Earth's full geopotential (EGM96 spherical harmonics, Cunningham/Pines
//     recursion) evaluated in an Earth-fixed frame and rotated back to ECI;
//   * Sun and Moon point-mass third-body perturbations;
//   * atmospheric drag (piecewise-exponential density, co-rotating atmosphere);
//   * solar radiation pressure with a cylindrical Earth-shadow model.
//
// The integration frame is the same TEME-as-pseudo-inertial frame produced by
// SGP4, so downstream consumers (observer look angles, geodetic conversion)
// need no changes. Earth-fixed rotation uses the application's GMST.
namespace ve {

    struct ForceParams {
        int grav_degree = 20;     // geopotential max degree (<= egm96::MAX_DEGREE)
        int grav_order  = 20;     // geopotential max order  (<= grav_degree)
        bool use_sun  = true;
        bool use_moon = true;
        bool use_drag = true;
        bool use_srp  = true;

        // Spacecraft physical properties. If mass/area are left <= 0 the model
        // derives an effective ballistic coefficient from the TLE B* term.
        double mass_kg      = 0.0;
        double drag_area_m2 = 0.0;
        double srp_area_m2  = 0.0;
        double Cd = 2.2;          // drag coefficient
        double Cr = 1.3;          // radiation-pressure coefficient
        double bstar = 0.0;       // TLE B* drag term [1/earth radii]
    };

    class ForceModel {
    public:
        explicit ForceModel(const ForceParams& p);

        // Total inertial acceleration at Julian date jd for ECI state (r,v) in km, km/s.
        Vector3 acceleration(double jd, const Vector3& r_eci, const Vector3& v_eci) const;

        // Effective ballistic / SRP area-to-mass actually in use (m^2/kg). Exposed
        // for diagnostics and tests. cdAoverM = Cd*A/m, crAoverM = Cr*A/m.
        double cdAoverM() const { return cd_area_over_m_; }
        double crAoverM() const { return cr_area_over_m_; }
        bool dragEnabled() const { return drag_enabled_; }

        // Piecewise-exponential atmospheric density [kg/m^3] at geometric
        // altitude [km] (Vallado, Fundamentals of Astrodynamics, Table 8-4).
        static double atmosphereDensity(double alt_km);

    private:
        Vector3 gravityAccel(double jd, const Vector3& r_eci) const;
        Vector3 thirdBodyAccel(const Vector3& r_eci, const Vector3& r_body, double gm) const;
        Vector3 dragAccel(const Vector3& r_eci, const Vector3& v_eci) const;
        Vector3 srpAccel(double jd, const Vector3& r_eci) const;

        ForceParams p_;
        int nmax_;
        int mmax_;
        std::vector<std::vector<double>> C_; // de-normalized cosine coeffs [n][m]
        std::vector<std::vector<double>> S_; // de-normalized sine   coeffs [n][m]
        double cd_area_over_m_ = 0.0;        // Cd*A/m [m^2/kg]
        double cr_area_over_m_ = 0.0;        // Cr*A/m [m^2/kg]
        bool drag_enabled_ = false;
    };
}
