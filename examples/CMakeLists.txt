# The programs of docs/hello-world.md and docs/tutorial.md.
foreach (example hello-world tutorial-writer tutorial-reader)
    add_executable(${example} ${example}.c)
    target_link_libraries(${example} PRIVATE mxl-monitoring-http)
endforeach ()

# Serves HTTP of its own, so it takes the Prometheus text without the endpoint.
add_executable(own-server own-server.c)
target_link_libraries(own-server PRIVATE mxl-monitoring-prometheus Threads::Threads)

if (MXL_MONITORING_OTLP)
    add_executable(beside-otlp beside-otlp.cpp)
    target_link_libraries(beside-otlp
            PRIVATE
                mxl-monitoring-otlp
                opentelemetry-cpp::otlp_http_metric_exporter
                opentelemetry-cpp::metrics
        )
    add_executable(own-instruments own-instruments.cpp)
    target_link_libraries(own-instruments
            PRIVATE
                mxl-monitoring
                opentelemetry-cpp::otlp_http_metric_exporter
                opentelemetry-cpp::otlp_http_log_record_exporter
                opentelemetry-cpp::logs
                opentelemetry-cpp::metrics
        )
endif ()
