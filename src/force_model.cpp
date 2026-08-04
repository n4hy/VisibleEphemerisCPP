// force_model.cpp - Total inertial acceleration on an Earth-orbiting satellite.
//
// Sums four contributions, all in km/s^2, in the TEME-as-pseudo-inertial frame:
//   1. Earth geopotential  - EGM96 spherical harmonics (Cunningham/Pines V,W
//      recursion). The position is rotated ECI->ECEF by GMST, the acceleration
//      is evaluated body-fixed, then rotated back. The central two-body term is
//      included via the C(0,0)=1 coefficient, so this single routine yields the
//      complete gravitational acceleration.
//   2. Third bodies        - Sun and Moon point-mass (direct + indirect terms).
//   3. Atmospheric drag    - piecewise-exponential density (Vallado Table 8-4),
//      atmosphere co-rotating with Earth; Cd*A/m from the TLE B* term.
//   4. Solar radiation pressure - inverse-square with a cylindrical Earth shadow.
//
// Normalized EGM96 coefficients (egm96_coeffs.hpp) are de-normalized once at
// construction. See force_model.hpp for the ForceParams switches.
#include "force_model.hpp"
#include "ephemeris.hpp"
#include "egm96_coeffs.hpp"
#include "earth_rotation.hpp"
#include <algorithm>
#include <cmath>

namespace ve {
    namespace {
        // SGP4/TLE B* reference air density: B* = rho0 * (Cd*A/m) / 2,
        // with rho0 = 0.15696615 kg/(m^2 * earth-radii). Hence Cd*A/m = 2*B*/rho0.
        constexpr double BSTAR_RHO0 = 0.15696615;

        // Fully-normalized -> unnormalized conversion factor for (n,m):
        //   N = sqrt( (2 - d0m) * (2n+1) * (n-m)! / (n+m)! )
        // Computed as a running product to avoid large factorials.
        double denormFactor(int n, int m) {
            double f = (m == 0) ? 1.0 : 2.0;
            f *= (2.0 * n + 1.0);
            for (int k = n - m + 1; k <= n + m; ++k) f /= static_cast<double>(k);
            return std::sqrt(f);
        }

        // Vallado exponential atmosphere bands: { base alt [km], rho0 [kg/m^3], scale height [km] }.
        struct AtmoBand { double h0, rho0, H; };
        constexpr AtmoBand ATMO[] = {
            {   0, 1.225e+00,   7.249}, {  25, 3.899e-02,   6.349}, {  30, 1.774e-02,   6.682},
            {  40, 3.972e-03,   7.554}, {  50, 1.057e-03,   8.382}, {  60, 3.206e-04,   7.714},
            {  70, 8.770e-05,   6.549}, {  80, 1.905e-05,   5.799}, {  90, 3.396e-06,   5.382},
            { 100, 5.297e-07,   5.877}, { 110, 9.661e-08,   7.263}, { 120, 2.438e-08,   9.473},
            { 130, 8.484e-09,  12.636}, { 140, 3.845e-09,  16.149}, { 150, 2.070e-09,  22.523},
            { 180, 5.464e-10,  29.740}, { 200, 2.789e-10,  37.105}, { 250, 7.248e-11,  45.546},
            { 300, 2.418e-11,  53.628}, { 350, 9.518e-12,  53.298}, { 400, 3.725e-12,  58.515},
            { 450, 1.585e-12,  60.828}, { 500, 6.967e-13,  63.822}, { 600, 1.454e-13,  71.835},
            { 700, 3.614e-14,  88.667}, { 800, 1.170e-14, 124.640}, { 900, 5.245e-15, 181.050},
            {1000, 3.019e-15, 268.000},
        };
        constexpr int NUM_ATMO = sizeof(ATMO) / sizeof(ATMO[0]);
    }

    double ForceModel::atmosphereDensity(double alt_km) {
        if (alt_km <= ATMO[0].h0) return ATMO[0].rho0;          // at/below sea level
        int i = NUM_ATMO - 1;                                    // default: top band
        for (int k = 0; k < NUM_ATMO - 1; ++k) {
            if (alt_km < ATMO[k + 1].h0) { i = k; break; }
        }
        return ATMO[i].rho0 * std::exp(-(alt_km - ATMO[i].h0) / ATMO[i].H);
    }

    ForceModel::ForceModel(const ForceParams& p) : p_(p) {
        nmax_ = std::min(p.grav_degree, egm96::MAX_DEGREE);
        if (nmax_ < 0) nmax_ = 0;
        mmax_ = std::min(p.grav_order, nmax_);
        if (mmax_ < 0) mmax_ = 0;

        // Build de-normalized coefficient arrays, with the central term C(0,0)=1.
        C_.assign(nmax_ + 1, std::vector<double>(nmax_ + 1, 0.0));
        S_.assign(nmax_ + 1, std::vector<double>(nmax_ + 1, 0.0));
        C_[0][0] = 1.0;
        for (int i = 0; i < egm96::NUM_COEFFS; ++i) {
            const auto& c = egm96::COEFFS[i];
            if (c.n > nmax_ || c.m > mmax_) continue;
            double f = denormFactor(c.n, c.m);
            C_[c.n][c.m] = c.C * f;
            S_[c.n][c.m] = c.S * f;
        }

        // Derive effective area-to-mass ratios.
        if (p_.mass_kg > 0.0 && p_.drag_area_m2 > 0.0)
            cd_area_over_m_ = p_.Cd * p_.drag_area_m2 / p_.mass_kg;
        else if (p_.bstar > 0.0)
            cd_area_over_m_ = 2.0 * p_.bstar / BSTAR_RHO0;     // from TLE B*
        else
            cd_area_over_m_ = 0.0;                              // unknown -> no drag

        drag_enabled_ = p_.use_drag && (cd_area_over_m_ > 0.0);

        if (p_.mass_kg > 0.0 && p_.srp_area_m2 > 0.0)
            cr_area_over_m_ = p_.Cr * p_.srp_area_m2 / p_.mass_kg;
        else if (cd_area_over_m_ > 0.0)
            cr_area_over_m_ = p_.Cr * (cd_area_over_m_ / p_.Cd); // share area, swap coeff
        else
            cr_area_over_m_ = p_.Cr * 0.02;                     // typical default A/m
    }

    Vector3 ForceModel::gravityAccel(double jd, const Vector3& r_eci) const {
        // Rotate ECI -> ECEF about z. Angle is GMST by default (TEME-consistent
        // with the rest of the tracker) or GAST = GMST + Eqn(Equinoxes) when
        // ForceParams::use_iau2000_rotation is set, matching IAU2000Rotation
        // in earth_rotation.hpp.
        double th = p_.use_iau2000_rotation ? gastFromJD(jd) : gmstFromJD(jd);
        double cth = std::cos(th), sth = std::sin(th);
        double x =  cth * r_eci.x + sth * r_eci.y;
        double y = -sth * r_eci.x + cth * r_eci.y;
        double z =  r_eci.z;

        const double R = egm96::R_REF_KM;
        const double GM = egm96::GM_KM3_S2;
        double r2 = x * x + y * y + z * z;
        double rho = R * R / r2;
        double x0 = R * x / r2, y0 = R * y / r2, z0 = R * z / r2;

        const int N = nmax_;
        // Cunningham auxiliary functions V, W up to degree N+1.
        constexpr int DIM = egm96::MAX_DEGREE + 2;
        double V[DIM][DIM] = {{0}};
        double W[DIM][DIM] = {{0}};

        V[0][0] = R / std::sqrt(r2);
        W[0][0] = 0.0;
        V[1][0] = z0 * V[0][0];
        W[1][0] = 0.0;
        for (int n = 2; n <= N + 1; ++n) {
            V[n][0] = ((2 * n - 1) * z0 * V[n - 1][0] - (n - 1) * rho * V[n - 2][0]) / n;
            W[n][0] = 0.0;
        }
        for (int m = 1; m <= N + 1; ++m) {
            V[m][m] = (2 * m - 1) * (x0 * V[m - 1][m - 1] - y0 * W[m - 1][m - 1]);
            W[m][m] = (2 * m - 1) * (x0 * W[m - 1][m - 1] + y0 * V[m - 1][m - 1]);
            if (m <= N) {
                V[m + 1][m] = (2 * m + 1) * z0 * V[m][m];
                W[m + 1][m] = (2 * m + 1) * z0 * W[m][m];
            }
            for (int n = m + 2; n <= N + 1; ++n) {
                V[n][m] = ((2 * n - 1) * z0 * V[n - 1][m] - (n + m - 1) * rho * V[n - 2][m]) / (n - m);
                W[n][m] = ((2 * n - 1) * z0 * W[n - 1][m] - (n + m - 1) * rho * W[n - 2][m]) / (n - m);
            }
        }

        double ax = 0.0, ay = 0.0, az = 0.0;
        for (int m = 0; m <= N; ++m) {
            for (int n = m; n <= N; ++n) {
                double Cnm = C_[n][m], Snm = S_[n][m];
                if (Cnm == 0.0 && Snm == 0.0) continue;
                if (m == 0) {
                    ax -= Cnm * V[n + 1][1];
                    ay -= Cnm * W[n + 1][1];
                    az -= (n + 1) * Cnm * V[n + 1][0];
                } else {
                    double fac = 0.5 * (n - m + 1) * (n - m + 2);
                    ax += 0.5 * (-Cnm * V[n + 1][m + 1] - Snm * W[n + 1][m + 1])
                        + fac * (Cnm * V[n + 1][m - 1] + Snm * W[n + 1][m - 1]);
                    ay += 0.5 * (-Cnm * W[n + 1][m + 1] + Snm * V[n + 1][m + 1])
                        + fac * (-Cnm * W[n + 1][m - 1] + Snm * V[n + 1][m - 1]);
                    az += (n - m + 1) * (-Cnm * V[n + 1][m] - Snm * W[n + 1][m]);
                }
            }
        }
        double scale = GM / (R * R);
        // Acceleration in ECEF, rotate back to ECI by -theta (theta = GMST
        // or GAST depending on use_iau2000_rotation, matching the forward
        // rotation used at the top of this function).
        double aex = scale * ax, aey = scale * ay, aez = scale * az;
        return { cth * aex - sth * aey, sth * aex + cth * aey, aez };
    }

    Vector3 ForceModel::thirdBodyAccel(const Vector3& r, const Vector3& s, double gm) const {
        Vector3 d = s - r;                       // satellite -> body
        double d2 = d.dot(d);
        double s2 = s.dot(s);
        double d3 = d2 * std::sqrt(d2);
        double s3 = s2 * std::sqrt(s2);
        return d * (gm / d3) - s * (gm / s3);    // direct + indirect terms
    }

    Vector3 ForceModel::dragAccel(const Vector3& r, const Vector3& v) const {
        double alt = r.magnitude() - EARTH_RADIUS_KM;
        double dens = atmosphereDensity(alt);
        if (dens <= 0.0) return {0, 0, 0};
        Vector3 omega{0.0, 0.0, EARTH_ROTATION_RATE};
        Vector3 v_rel = v - omega.cross(r);      // km/s, atmosphere co-rotates
        double vmag_ms = v_rel.magnitude() * 1000.0;
        if (vmag_ms <= 0.0) return {0, 0, 0};
        // a[m/s^2] = -0.5 * rho * (Cd*A/m) * |v_rel| * v_rel
        double coef = -0.5 * dens * cd_area_over_m_ * vmag_ms; // applied to v_rel [m/s]
        Vector3 a_ms = (v_rel * 1000.0) * coef;                // m/s^2
        return a_ms * 1e-3;                                    // km/s^2
    }

    Vector3 ForceModel::srpAccel(const Vector3& r, const Vector3& r_sun) const {
        // Cylindrical Earth-shadow test (penumbra neglected).
        Vector3 s_hat = r_sun.normalize();
        double r_par = r.dot(s_hat);
        if (r_par < 0.0) {
            Vector3 perp = r - s_hat * r_par;
            if (perp.magnitude() < EARTH_RADIUS_KM) return {0, 0, 0}; // in umbra
        }
        Vector3 d = r - r_sun;                   // Sun -> satellite (push direction)
        double dmag = d.magnitude();
        double au_ratio = ASTRONOMICAL_UNIT_KM / dmag;
        double P = SOLAR_PRESSURE_1AU * au_ratio * au_ratio;  // N/m^2 at satellite
        double a_ms = P * cr_area_over_m_;                    // m/s^2
        return (d * (1.0 / dmag)) * (a_ms * 1e-3);            // km/s^2 away from Sun
    }

    Vector3 ForceModel::acceleration(double jd, const Vector3& r, const Vector3& v) const {
        Vector3 a = gravityAccel(jd, r);
        // Sun position feeds both the third-body term and SRP; evaluate it once.
        // Use the MOD-rotated (TEME-equivalent) form so the third-body and SRP
        // vectors live in the same frame as the state r,v being integrated.
        // Bare sunPositionECI would return J2000 -- see ephemeris.hpp.
        bool need_sun = p_.use_sun || p_.use_srp;
        Vector3 r_sun = need_sun ? sunPositionTEME(jd) : Vector3{0, 0, 0};
        if (p_.use_sun)  a = a + thirdBodyAccel(r, r_sun, GM_SUN);
        if (p_.use_moon) a = a + thirdBodyAccel(r, moonPositionTEME(jd), GM_MOON);
        if (drag_enabled_) a = a + dragAccel(r, v);
        if (p_.use_srp)  a = a + srpAccel(r, r_sun);
        return a;
    }
}
