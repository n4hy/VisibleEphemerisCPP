#include <iostream>
#include <cassert>
#include <cmath>
#include "../include/visibility.hpp"
#include "../include/types.hpp"

using namespace ve;

void test_flare_visible() {
    // 1. Setup Perfect Geometry for Flare
    // Earth Center = 0,0,0
    // Sat Position (S) = (0, 0, 7000 km) (Altitude ~622km)
    // Mirror Normal (N) points to Earth Center: (0, 0, -1)

    // We want Reflection (R) to hit Observer.
    // Let Observer (O) be at (0, 0, 6378 km) (Directly under sat).
    // Vector V (Obs-Sat) = (0,0, -622). Normalized: (0,0,-1).

    // For Reflection R to be (0,0,-1), and R = I - 2(I.N)N
    // If I = (0,0,-1) (Sun directly above), then N=(0,0,-1).
    // I.N = 1.
    // R = (0,0,-1) - 2(1)(0,0,-1) = (0,0,-1) - (0,0,-2) = (0,0,1).
    // Wait. If Sun is directly ABOVE sat, light goes DOWN (0,0,-1).
    // Mirror faces DOWN.
    // Light hits Back of mirror?
    // checkFlare logic: I.N < 0 (Opposing) is required.
    // I=(0,0,-1), N=(0,0,-1). I.N = 1. Positive. Returns 0. Correct.

    // We need Sun "Below" the Sat for light to hit the "Nadir-Facing" surface?
    // My logic was: I.dot(N) < 0 means light opposes normal.
    // Normal points DOWN.
    // If I points UP (Sun below), I.dot(N) < 0.
    // Let Sun be at (0, 0, -Infinity).
    // I = (0, 0, 1). (Up).
    // N = (0, 0, -1). (Down).
    // I.N = -1. Good.
    // R = I - 2(-1)N = I + 2N = (0,0,1) + 2(0,0,-1) = (0,0,-1).
    // Reflection goes DOWN.
    // Obs is at (0, 0, 6378). Sat at (0,0,7000).
    // Obs-Sat Vector = (0,0,-1).
    // R (0,0,-1) matches V (0,0,-1).
    // Angle = 0.
    // Should be HIT (2).

    Vector3 sat = {0, 0, 7000};
    Vector3 obs = {0, 0, 6378};
    Vector3 sun = {0, 0, -150000000}; // Sun "Below"

    // Twilight check: Sun Elevation at Obs.
    // Obs = (0,0,6378). Up is (0,0,1).
    // Sun is (0,0,-Big).
    // Sun Vector from Obs = (0,0,-1).
    // Angle with Up = 180 deg.
    // Elevation = 90 - 180 = -90 deg.
    // Deep night. Valid.

    int res = VisibilityCalculator::checkFlare(sat, obs, sun, 622.0);
    std::cout << "Test 1 (Direct Nadir Flare): " << res << " (Expected 2)" << std::endl;
    assert(res == 2);
}

void test_flare_miss() {
    Vector3 sat = {0, 0, 7000};
    Vector3 obs = {0, 0, 6378};
    // Sun slightly off axis
    // Rotate Sun by 2 degrees.
    // I needs to rotate so R rotates.
    // If I rotates by 2 deg, R rotates by 2 deg (mirror fixed).
    // Angle diff = 2 deg. > 1.0 deg. Should be 0.

    double ang = 2.0 * DEG2RAD;
    Vector3 sun = {150000000 * std::sin(ang), 0, -150000000 * std::cos(ang)};

    int res = VisibilityCalculator::checkFlare(sat, obs, sun, 622.0);
    std::cout << "Test 2 (Miss): " << res << " (Expected 0)" << std::endl;
    assert(res == 0);
}

void test_flare_near() {
    Vector3 sat = {0, 0, 7000};
    Vector3 obs = {0, 0, 6378};
    // Rotate Sun by 0.7 degrees.
    // Angle diff = 0.7 deg. Should be 1 (Near).

    double ang = 0.7 * DEG2RAD;
    Vector3 sun = {150000000 * std::sin(ang), 0, -150000000 * std::cos(ang)};

    int res = VisibilityCalculator::checkFlare(sat, obs, sun, 622.0);
    std::cout << "Test 3 (Near): " << res << " (Expected 1)" << std::endl;
    assert(res == 1);
}

void test_not_leo() {
    Vector3 sat = {0, 0, 10000}; // Apogee 3622 > 1000
    Vector3 obs = {0, 0, 6378};
    Vector3 sun = {0, 0, -150000000};

    int res = VisibilityCalculator::checkFlare(sat, obs, sun, 3622.0);
    std::cout << "Test 4 (High Orbit): " << res << " (Expected 0)" << std::endl;
    assert(res == 0);
}

void test_daylight() {
    Vector3 sat = {0, 0, 7000};
    Vector3 obs = {0, 0, 6378};
    // Sun Above Obs
    Vector3 sun = {0, 0, 150000000};

    // Sun El at Obs = 90 deg.
    // Should fail twilight check.

    int res = VisibilityCalculator::checkFlare(sat, obs, sun, 622.0);
    std::cout << "Test 5 (Daylight): " << res << " (Expected 0)" << std::endl;
    assert(res == 0);
}

int main() {
    test_flare_visible();
    test_flare_miss();
    test_flare_near();
    test_not_leo();
    test_daylight();
    std::cout << "ALL TESTS PASSED" << std::endl;
    return 0;
}
