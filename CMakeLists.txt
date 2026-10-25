cmake_minimum_required(VERSION 3.24)

project(mxl-monitoring
        VERSION 0.2.0 # x-release-please-version
        LANGUAGES C CXX
    )

# Built either inside a dmf-mxl/mxl tree, where the mxl target exists, or on
# its own against an installed libmxl found through its CMake package.
if (TARGET mxl)
    set(MXL_MONITORING_MXL mxl)
else ()
    find_package(mxl CONFIG REQUIRED)
    set(MXL_MONITORING_MXL mxl::mxl)
endif ()

# Header-only, and shipped without a CMake package.
if (NOT TARGET picojson::picojson)
    find_path(MXL_MONITORING_PICOJSON_DIR NAMES picojson/picojson.h REQUIRED)
    add_library(picojson::picojson INTERFACE IMPORTED)
    set_target_properties(picojson::picojson PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${MXL_MONITORING_PICOJSON_DIR}")
endif ()

option(MXL_MONITORING_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})

# Static, so a process links it into itself and libmxl.so stays the only
# library every process on a domain has to load identically.
add_library(mxl-monitoring STATIC)
add_library(mxl-monitoring::mxl-monitoring ALIAS mxl-monitoring)
target_sources(mxl-monitoring
        PRIVATE
            src/Content.cpp
            src/Events.cpp
            src/Monitoring.cpp
            src/Reader.cpp
    )
target_compile_features(mxl-monitoring PUBLIC cxx_std_20)
target_include_directories(mxl-monitoring
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
target_link_libraries(mxl-monitoring
        PUBLIC
            ${MXL_MONITORING_MXL}
        PRIVATE
            picojson::picojson
    )

add_library(mxl-monitoring-prometheus STATIC)
add_library(mxl-monitoring::prometheus ALIAS mxl-monitoring-prometheus)
target_sources(mxl-monitoring-prometheus PRIVATE src/Prometheus.cpp)
target_link_libraries(mxl-monitoring-prometheus PUBLIC mxl-monitoring)

foreach (target IN ITEMS mxl-monitoring mxl-monitoring-prometheus)
    set_target_properties(${target}
            PROPERTIES
                POSITION_INDEPENDENT_CODE ON
                C_EXTENSIONS OFF
                CXX_EXTENSIONS OFF
        )
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach ()

if (MXL_MONITORING_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
