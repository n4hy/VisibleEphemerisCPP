cmake_minimum_required(VERSION 3.14)

# Prefer Clang (the project's reference compiler on Ubuntu) when it is installed,
# but fall back to the system default (e.g. GCC) so the build works without Clang.
# An explicit -DCMAKE_CXX_COMPILER=... always takes precedence.
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(CLANG_C_BIN clang)
    find_program(CLANG_CXX_BIN clang++)
    if(CLANG_C_BIN AND CLANG_CXX_BIN)
        set(CMAKE_C_COMPILER "${CLANG_C_BIN}")
        set(CMAKE_CXX_COMPILER "${CLANG_CXX_BIN}")
    endif()
endif()

project(VisibleEphemeris)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_HAMLIB "Enable Hamlib support" ON)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(Curses REQUIRED) 
find_package(PkgConfig REQUIRED)

if(ENABLE_HAMLIB)
    pkg_check_modules(HAMLIB hamlib)
    if(HAMLIB_FOUND)
        add_compile_definitions(ENABLE_HAMLIB)
        include_directories(${HAMLIB_INCLUDE_DIRS})
    else()
        message(WARNING "Hamlib not found, disabling Hamlib support.")
        set(ENABLE_HAMLIB OFF)
    endif()
endif()

set(USER_HOME $ENV{HOME})
set(PATHS_TO_CHECK "${USER_HOME}/sgp4/build/install" "/usr/local" "/usr")

message(STATUS "Searching for SGP4 Library...")
set(SGP4_FOUND FALSE)

foreach(ROOT_PATH ${PATHS_TO_CHECK})
    if(NOT SGP4_FOUND)
        find_library(SGP4_LIB_TEMP NAMES sgp4s libsgp4s PATHS ${ROOT_PATH}/lib ${ROOT_PATH}/lib64 NO_DEFAULT_PATH)
        find_path(SGP4_INC_TEMP NAMES Tle.h SGP4.h PATHS ${ROOT_PATH}/include/libsgp4 ${ROOT_PATH}/include NO_DEFAULT_PATH)
        if(SGP4_LIB_TEMP AND SGP4_INC_TEMP)
            set(SGP4_FOUND TRUE)
            set(SGP4_LIB ${SGP4_LIB_TEMP})
            set(SGP4_INCLUDE_DIR ${SGP4_INC_TEMP})
            message(STATUS "  [FOUND] SGP4 in ${ROOT_PATH}")
        endif()
    endif()
endforeach()

if(NOT SGP4_FOUND)
    message(FATAL_ERROR "Could not find libsgp4s.so.")
endif()

set(SOURCES
    src/main.cpp
    src/satellite.cpp
    src/ephemeris.cpp
    src/force_model.cpp
    src/numerical_propagator.cpp
    src/observer.cpp
    src/visibility.cpp
    src/pass_predictor.cpp
    src/tle_manager.cpp
    src/spacetrack_client.cpp
    src/display.cpp
    src/web_server.cpp
    src/text_server.cpp
    src/physics_server.cpp
    src/config_manager.cpp
    src/logger.cpp
    src/rotator.cpp
)

include_directories(include)
include_directories(${SGP4_INCLUDE_DIR})

add_executable(VisibleEphemeris ${SOURCES})

set(LIBS ${SGP4_LIB} CURL::libcurl Threads::Threads ${CURSES_LIBRARIES})
if(ENABLE_HAMLIB)
    list(APPEND LIBS ${HAMLIB_LIBRARIES})
endif()

target_link_libraries(VisibleEphemeris PRIVATE ${LIBS})

# Installation
install(TARGETS VisibleEphemeris DESTINATION bin)

# ----------------------------------------------------------------------------
# Optional unit tests:  cmake .. -DBUILD_TESTS=ON  &&  ctest
# ----------------------------------------------------------------------------
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_executable(test_hpop
        unittests/test_hpop.cpp
        src/ephemeris.cpp
        src/force_model.cpp
        src/numerical_propagator.cpp
        src/logger.cpp)
    target_include_directories(test_hpop PRIVATE include ${SGP4_INCLUDE_DIR})
    target_link_libraries(test_hpop PRIVATE ${SGP4_LIB})
    add_test(NAME hpop COMMAND test_hpop)

    add_executable(test_earth_rotation unittests/test_earth_rotation.cpp)
    target_include_directories(test_earth_rotation PRIVATE include)
    add_test(NAME earth_rotation COMMAND test_earth_rotation)
endif()

# ----------------------------------------------------------------------------
# Optional examples:  cmake .. -DBUILD_EXAMPLES=ON  &&  cmake --build . --target compare_iss
# ----------------------------------------------------------------------------
option(BUILD_EXAMPLES "Build example programs" OFF)
if(BUILD_EXAMPLES)
    add_executable(compare_iss
        examples/compare_iss.cpp
        src/ephemeris.cpp
        src/force_model.cpp
        src/numerical_propagator.cpp
        src/logger.cpp)
    target_include_directories(compare_iss PRIVATE include ${SGP4_INCLUDE_DIR})
    target_link_libraries(compare_iss PRIVATE ${SGP4_LIB})
endif()

# ----------------------------------------------------------------------------
# Optional: Orbit-determination (OD) subsystem -- SRUKF + RTS smoother +
# Doppler measurement model, consuming the sibling NLF and OptMathKernels
# libraries. See docs/orbit_determination.md for the design specification.
#
#   cmake .. -DBUILD_OD=ON \
#            -Dnlf_DIR=/home/n4hy/Modern-Computational-Nonlinear-Filtering/install/lib/cmake/nlf \
#            -DOptMathKernels_DIR=/home/n4hy/Modern-Computational-Nonlinear-Filtering/install/lib/cmake/OptMathKernels
# ----------------------------------------------------------------------------
option(BUILD_OD "Build the orbit-determination (OD) subsystem"       OFF)
option(BUILD_OD_BENCHMARKS "Build the OD benchmarks (implies BUILD_OD)" OFF)
if(BUILD_OD_BENCHMARKS AND NOT BUILD_OD)
    set(BUILD_OD ON)
endif()

if(BUILD_OD)
    # Default hints so a checkout-next-to-checkout dev layout Just Works.
    if(NOT DEFINED nlf_DIR)
        set(nlf_DIR "${USER_HOME}/Modern-Computational-Nonlinear-Filtering/install/lib/cmake/nlf"
            CACHE PATH "Directory containing nlfConfig.cmake")
    endif()
    if(NOT DEFINED OptMathKernels_DIR)
        # OptMathKernels is exported via NLF's install tree as well as via
        # /usr/local; prefer NLF's install to avoid version skew.
        set(OptMathKernels_DIR "${USER_HOME}/Modern-Computational-Nonlinear-Filtering/install/lib/cmake/OptMathKernels"
            CACHE PATH "Directory containing OptMathKernelsConfig.cmake")
    endif()

    find_package(Eigen3 3.4 REQUIRED)
    find_package(OptMathKernels REQUIRED)
    find_package(nlf REQUIRED)

    add_library(ve_od STATIC
        src/od/od_types.cpp
        src/od/od_dynamics.cpp
        src/od/tle_to_state.cpp
        src/od/doppler_measurement.cpp
        src/od/od_smoother.cpp
        src/force_model.cpp
        src/ephemeris.cpp
        src/logger.cpp)
    target_include_directories(ve_od PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${SGP4_INCLUDE_DIR})
    target_link_libraries(ve_od PUBLIC
        Eigen3::Eigen
        nlf::nlf
        OptMathKernels::OptMathKernels
        ${SGP4_LIB})
    # Force C++20 for ve_od because NLF uses <numbers> and other C++20 constructs.
    target_compile_features(ve_od PUBLIC cxx_std_20)

    # Unit tests
    if(BUILD_TESTS)
        add_executable(test_od_frames unittests/test_od_frames.cpp)
        target_link_libraries(test_od_frames PRIVATE ve_od)
        add_test(NAME od_frames COMMAND test_od_frames)

        add_executable(test_od_doppler unittests/test_od_doppler.cpp)
        target_link_libraries(test_od_doppler PRIVATE ve_od)
        add_test(NAME od_doppler COMMAND test_od_doppler)

        add_executable(test_od_srukf_linear unittests/test_od_srukf_linear.cpp)
        target_link_libraries(test_od_srukf_linear PRIVATE ve_od)
        add_test(NAME od_srukf_linear COMMAND test_od_srukf_linear)

        add_executable(test_od_propagator unittests/test_od_propagator.cpp)
        target_link_libraries(test_od_propagator PRIVATE ve_od)
        add_test(NAME od_propagator COMMAND test_od_propagator)

        add_executable(test_od_gate_guard unittests/test_od_gate_guard.cpp)
        target_link_libraries(test_od_gate_guard PRIVATE ve_od)
        add_test(NAME od_gate_guard COMMAND test_od_gate_guard)
    endif()

    # Benchmarks (opt-in separately -- can be slow).
    if(BUILD_OD_BENCHMARKS)
        add_executable(bench_synthetic     benchmarks/bench_synthetic.cpp)
        target_link_libraries(bench_synthetic PRIVATE ve_od)

        add_executable(bench_iss_pass      benchmarks/bench_iss_pass.cpp)
        target_link_libraries(bench_iss_pass PRIVATE ve_od)
    endif()
endif()

# ----------------------------------------------------------------------------
# Optional: pybind11 module exposing the High-Precision Orbit Propagator.
#   cmake .. -DBUILD_PYTHON_BINDINGS=ON
# Produces an importable `ve_hpop` Python extension. pybind11 is located via
# find_package, then an installed pip package, and finally fetched from GitHub.
# ----------------------------------------------------------------------------
option(BUILD_PYTHON_BINDINGS "Build the ve_hpop pybind11 module" OFF)
if(BUILD_PYTHON_BINDINGS)
    find_package(pybind11 CONFIG QUIET)
    if(NOT pybind11_FOUND)
        find_program(PYTHON_EXECUTABLE NAMES python3 python)
        if(PYTHON_EXECUTABLE)
            execute_process(
                COMMAND ${PYTHON_EXECUTABLE} -m pybind11 --cmakedir
                OUTPUT_VARIABLE PYBIND11_CMAKEDIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE PYBIND11_PROBE)
            if(PYBIND11_PROBE EQUAL 0 AND PYBIND11_CMAKEDIR)
                find_package(pybind11 CONFIG QUIET PATHS ${PYBIND11_CMAKEDIR})
            endif()
        endif()
    endif()
    if(NOT pybind11_FOUND)
        message(STATUS "pybind11 not found locally; fetching from GitHub...")
        include(FetchContent)
        FetchContent_Declare(pybind11
            GIT_REPOSITORY https://github.com/pybind/pybind11.git
            GIT_TAG v2.13.6)
        FetchContent_MakeAvailable(pybind11)
    endif()

    pybind11_add_module(ve_hpop
        src/python_bindings.cpp
        src/ephemeris.cpp
        src/force_model.cpp
        src/numerical_propagator.cpp
        src/logger.cpp)
    target_include_directories(ve_hpop PRIVATE include ${SGP4_INCLUDE_DIR})
    target_link_libraries(ve_hpop PRIVATE ${SGP4_LIB})
    message(STATUS "Python bindings (ve_hpop) will be built.")
endif()
