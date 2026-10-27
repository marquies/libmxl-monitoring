// A minimal HTTP endpoint for a process that serves no HTTP of its own.
// GET /metrics answers with the Prometheus text of every flow, GET /healthz
// with 200 while the endpoint runs. One thread answers one request at a
// time, without keep-alive; reading a request and sending its answer each
// get 200 ms, so a client that sends nothing or reads slowly cannot hold up
// the next. A process with an HTTP server of its own serves
// mxlMonitoringPrometheusRender from it instead.

#pragma once

#include <stdint.h>
#include "monitoring.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct mxlMonitoringHttp_t* mxlMonitoringHttp;

    // Listens on address, NULL for every interface, and port, 0 for one the
    // system picks, and starts the thread that answers. MXL_ERR_INVALID_ARG
    // for a NULL monitoring or http, MXL_ERR_UNKNOWN when the address cannot
    // be resolved or bound; errno says why. The monitoring must outlive the
    // endpoint.
    mxlStatus mxlMonitoringHttpStart(mxlMonitoring monitoring, char const* address, uint16_t port, mxlMonitoringHttp* http);

    // The port listened on, which tells a port of 0 the system picked.
    uint16_t mxlMonitoringHttpPort(mxlMonitoringHttp http);

    // Stops answering, waits for the thread and closes the socket.
    void mxlMonitoringHttpStop(mxlMonitoringHttp http);

#ifdef __cplusplus
}
#endif
