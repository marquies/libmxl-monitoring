if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 REQUIRED)
endif ()

add_executable(mxl-monitoring-tests)
target_sources(mxl-monitoring-tests
        PRIVATE
            c_headers.c
            test_classify.cpp
            test_concurrency.cpp
            test_content.cpp
            test_events.cpp
            test_prometheus.cpp
            test_reader.cpp
            test_recording.cpp
    )
target_compile_features(mxl-monitoring-tests PRIVATE cxx_std_20 c_std_11)
set_source_files_properties(c_headers.c PROPERTIES COMPILE_OPTIONS "-Wpedantic;-Werror")
target_compile_options(mxl-monitoring-tests PRIVATE -Wall -Wextra)
target_link_libraries(mxl-monitoring-tests
        PRIVATE
            mxl-monitoring
            mxl-monitoring-prometheus
            Catch2::Catch2WithMain
    )
target_include_directories(mxl-monitoring-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
