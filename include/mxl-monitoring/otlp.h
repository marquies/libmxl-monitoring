// Export over OTLP/HTTP to an OpenTelemetry Collector: every flow's counts,
// states and latencies as metrics, every event as a log record. Configured
// through the standard OTEL_* environment variables only, read on start:
// OTEL_EXPORTER_OTLP_ENDPOINT (and the per-signal _METRICS_ and _LOGS_
// variants), _HEADERS, _TIMEOUT, OTEL_EXPORTER_OTLP_PROTOCOL (http/protobuf or
// http/json), OTEL_METRIC_EXPORT_INTERVAL, OTEL_SERVICE_NAME and
// OTEL_RESOURCE_ATTRIBUTES.
//
// Metrics are built from snapshots on a thread of the exporter, every export
// interval, with cumulative temporality. Log records come from the events
// mxlMonitoringPoll delivers, so a process that wants them polls.

#pragma once

#include "monitoring.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct mxlMonitoringOtlp_t* mxlMonitoringOtlp;

    // Starts exporting. serviceName is service.name where neither
    // OTEL_SERVICE_NAME nor OTEL_RESOURCE_ATTRIBUTES sets it, and may be NULL.
    // MXL_ERR_INVALID_ARG for a NULL monitoring or otlp, MXL_ERR_UNKNOWN when
    // the exporters cannot be created. The monitoring must outlive the
    // exporter. A collector that cannot be reached is retried at every
    // interval and does not fail the start.
    mxlStatus mxlMonitoringOtlpStart(mxlMonitoring monitoring, char const* serviceName, mxlMonitoringOtlp* otlp);

    // Exports once more, sends the log records still queued, and stops.
    void mxlMonitoringOtlpStop(mxlMonitoringOtlp otlp);

#ifdef __cplusplus
}
#endif
