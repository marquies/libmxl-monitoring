// Prometheus text exposition of every flow, for a process to serve on its own
// /metrics endpoint.
//
// Readers carry the metric names and label values mxl-reader has served, so
// dashboards and alerts built on them keep working, except for
// access_losses_total, which mxl-reader called lost_total. Writers and
// replication sources and targets follow the same scheme with the label
// values the OTLP exporter uses. The prefix names the role: mxl_reader,
// mxl_writer, mxl_replication_source, mxl_replication_target.
//
// Where the values differ from mxl-reader's: an open that fails because the
// flow's data was replaced is in state flow_not_found rather than flow_error,
// and an expected rate matches any equal fraction (100/2 is 50/1).

#pragma once

#include <stddef.h>
#include "monitoring.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Renders into buffer, NUL-terminated, and sets *size to the bytes
    // needed including the NUL. A NULL or too small buffer returns
    // MXL_ERR_INVALID_ARG with *size set, as mxlGetFlowDef does.
    mxlStatus mxlMonitoringPrometheusRender(mxlMonitoring monitoring, char* buffer, size_t* size);

#ifdef __cplusplus
}
#endif
