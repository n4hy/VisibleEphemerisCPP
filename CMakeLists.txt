cmake_minimum_required(VERSION 3.14)
project(VisibleEphemeris)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(Curses REQUIRED) 
find_package(PkgConfig REQUIRED)
pkg_check_modules(HAMLIB REQUIRED hamlib)

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
    src/observer.cpp 
    src/visibility.cpp 
    src/pass_predictor.cpp 
    src/tle_manager.cpp
    src/display.cpp
    src/web_server.cpp
    src/text_server.cpp
    src/config_manager.cpp
    src/logger.cpp
    src/rotator.cpp
    src/rig_control.cpp
    src/frequency_manager.cpp
)

include_directories(include)
include_directories(${SGP4_INCLUDE_DIR})
include_directories(${HAMLIB_INCLUDE_DIRS})

add_executable(VisibleEphemeris ${SOURCES})
target_link_libraries(VisibleEphemeris PRIVATE ${SGP4_LIB} CURL::libcurl Threads::Threads ${CURSES_LIBRARIES} ${HAMLIB_LIBRARIES})
