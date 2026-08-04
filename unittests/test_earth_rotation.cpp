//
// test_earth_rotation -- minimal smoke tests for the pluggable
//                        Earth-rotation interface in
//                        include/earth_rotation.hpp.
//
// Build (standalone, matches the existing test_flare.cpp style):
//   clang++ -std=c++17 -O2 -I../include unittests/test_earth_rotation.cpp
//   ../src/visibility.cpp -o test_earth_rotation
//
// Run:
//   ./test_earth_rotation
//

#include <iostream>
#include <cassert>
#include <cmath>
#include <chrono>
#include "../include/earth_rotation.hpp"
#include "../include/types.hpp"

using namespace ve;

static void expect_close(double a, double b, double tol, const char* name) {
    if (std::abs(a - b) > tol) {
        std::cerr << "FAIL " << name << ": |" << a << " - " << b << "| > " << tol << "\n";
        std::abort();
    }
}

// J2000 epoch as a system_clock TimePoint:
// 2000-01-01 12:00:00 UTC (TT actually, but for our visibility-grade
// precision this is fine to within seconds).
static TimePoint j2000() {
    std::tm t{};
    t.tm_year = 100;   // 2000 - 1900
    t.tm_mon  = 0;     // January
    t.tm_mday = 1;
    t.tm_hour = 12;
    t.tm_min  = 0;
    t.tm_sec  = 0;
    std::time_t tt = timegm(&t);
    return Clock::from_time_t(tt);
}

// Matrix orthonormality: R * R^T == I and det(R) == +1.
static void check_orthonormal(const Rotation3& R, const char* name) {
    double tol = 1e-12;
    // Diagonal of R R^T == 1, off-diagonal == 0.
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = R.m[i][0]*R.m[j][0]
                     + R.m[i][1]*R.m[j][1]
                     + R.m[i][2]*R.m[j][2];
            double expected = (i == j) ? 1.0 : 0.0;
            if (std::abs(s - expected) > tol) {
                std::cerr << "FAIL " << name << ": R R^T not identity "
                          << "(" << i << "," << j << ") = " << s << "\n";
                std::abort();
            }
        }
    }
    // det(R) == 1 (z-rotation is always det +1).
    double det = R.m[0][0]*(R.m[1][1]*R.m[2][2] - R.m[1][2]*R.m[2][1])
               - R.m[0][1]*(R.m[1][0]*R.m[2][2] - R.m[1][2]*R.m[2][0])
               + R.m[0][2]*(R.m[1][0]*R.m[2][1] - R.m[1][1]*R.m[2][0]);
    expect_close(det, 1.0, tol, name);
}

static void test_gmst_orthonormal() {
    TimePoint t = j2000();
    Rotation3 R = GmstRotation().R_eci_to_ecef(t);
    check_orthonormal(R, "GmstRotation @ J2000");
    std::cout << "Test 1 (GmstRotation orthonormal): OK\n";
}

static void test_iau_orthonormal() {
    TimePoint t = j2000();
    Rotation3 R = IAU2000Rotation().R_eci_to_ecef(t);
    check_orthonormal(R, "IAU2000Rotation @ J2000");
    std::cout << "Test 2 (IAU2000Rotation orthonormal): OK\n";
}

static void test_iau_close_to_gmst() {
    // The IAU rotation differs from the GMST rotation by the equation
    // of the equinoxes, which is at most ~1.3 as = 6e-6 rad.  The
    // resulting Frobenius distance between the two rotation matrices
    // is bounded above by 2 * sqrt(2) * |E_e|, so well under 1e-4.
    TimePoint t = j2000();
    Rotation3 G = GmstRotation().R_eci_to_ecef(t);
    Rotation3 I = IAU2000Rotation().R_eci_to_ecef(t);
    double frob = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double d = G.m[i][j] - I.m[i][j];
            frob += d * d;
        }
    frob = std::sqrt(frob);
    if (frob > 1e-4) {
        std::cerr << "FAIL IAU vs GMST too far apart: " << frob << "\n";
        std::abort();
    }
    std::cout << "Test 3 (IAU ~ GMST near J2000, ||delta||=" << frob << "): OK\n";
}

static void test_round_trip() {
    // Rotate a vector ECI -> ECEF -> ECI through the IAU rotator and
    // expect to recover the original to machine precision.
    TimePoint t = j2000();
    IAU2000Rotation rot;
    Vector3 v_eci = { 7000.0, 1000.0, -300.0 };
    Rotation3 R = rot.R_eci_to_ecef(t);
    Vector3 v_ecef = R * v_eci;
    Vector3 v_back = R.transpose_apply(v_ecef);
    expect_close(v_back.x, v_eci.x, 1e-9, "round_trip x");
    expect_close(v_back.y, v_eci.y, 1e-9, "round_trip y");
    expect_close(v_back.z, v_eci.z, 1e-9, "round_trip z");
    std::cout << "Test 4 (ECI->ECEF->ECI round trip): OK\n";
}

static void test_default_is_gmst() {
    // The defaultRotation() must behave identically to a fresh
    // GmstRotation() at the same epoch.
    TimePoint t = j2000();
    Rotation3 A = defaultRotation().R_eci_to_ecef(t);
    Rotation3 B = GmstRotation().R_eci_to_ecef(t);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            expect_close(A.m[i][j], B.m[i][j], 0.0, "default == Gmst");
    std::cout << "Test 5 (defaultRotation == GmstRotation): OK\n";
}

int main() {
    test_gmst_orthonormal();
    test_iau_orthonormal();
    test_iau_close_to_gmst();
    test_round_trip();
    test_default_is_gmst();
    std::cout << "ALL TESTS PASSED" << std::endl;
    return 0;
}
