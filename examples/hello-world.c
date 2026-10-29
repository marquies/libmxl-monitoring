// Counts what a reader of one MXL video flow sees and serves it on /metrics.
// The program of docs/hello-world.md.
//
//   hello-world <domain> <flow id> [<metrics port>]

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/mxl.h>
#include <mxl/time.h>
#include <mxl-monitoring/http.h>
#include <mxl-monitoring/monitoring.h>
#include <mxl-monitoring/reader.h>

static volatile sig_atomic_t stopping = 0;

static void onSignal(int signal)
{
    (void)signal;
    stopping = 1;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <domain> <flow id> [<metrics port>]\n", argv[0]);
        return 2;
    }
    char const* domain = argv[1];
    char const* flowId = argv[2];
    uint16_t const port = (uint16_t)(argc > 3 ? atoi(argv[3]) : 9100);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    // One context per process, one handle per flow it reads or writes.
    mxlMonitoring monitoring;
    mxlMonitoringFlow flow;
    mxlMonitoringCreate(NULL, &monitoring);
    mxlMonitoringAddFlow(monitoring, flowId, MXL_MONITORING_ROLE_READER, &flow);

    // GET /metrics on port: every count, as Prometheus text.
    mxlMonitoringHttp http;
    if (mxlMonitoringHttpStart(monitoring, NULL, port, &http) != MXL_STATUS_OK)
    {
        fprintf(stderr, "cannot serve metrics on port %u\n", port);
        return 1;
    }

    // The wrappers call libmxl and record what it returned on the flow.
    mxlInstance instance = mxlMonitoringCreateInstance(flow, domain, "");
    mxlFlowReader reader;
    if (instance == NULL || mxlMonitoringCreateFlowReader(flow, instance, "", &reader) != MXL_STATUS_OK)
    {
        fprintf(stderr, "cannot open flow %s in %s; curl localhost:%u/metrics says why\n", flowId, domain, port);
        return 1;
    }
    mxlFlowInfo info;
    mxlFlowReaderGetInfo(reader, &info);
    mxlRational const rate = info.config.common.grainRate;

    // Read every grain as the media clock makes it due.
    uint64_t index = mxlGetCurrentIndex(&rate);
    while (!stopping)
    {
        mxlGrainInfo grain;
        uint8_t* payload;
        mxlStatus const status = mxlMonitoringFlowReaderGetGrain(flow, reader, index, 200000000, &grain, &payload);
        if (status == MXL_STATUS_OK)
        {
            ++index;
        }
        else if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
        {
            // Overwritten before it was read: jump to the head, and count what
            // was jumped over.
            uint64_t const head = mxlGetCurrentIndex(&rate);
            mxlMonitoringRecordSkipped(flow, head - index);
            index = head;
        }
        else if (status != MXL_ERR_OUT_OF_RANGE_TOO_EARLY && status != MXL_ERR_TIMEOUT)
        {
            fprintf(stderr, "flow %s: read failed with status %d\n", flowId, (int)status);
            break;
        }
        // Turns the counts into events; more often than once a second.
        mxlMonitoringPoll(monitoring);
    }

    mxlReleaseFlowReader(instance, reader);
    mxlDestroyInstance(instance);
    mxlMonitoringHttpStop(http);
    mxlMonitoringDestroy(monitoring);
    return 0;
}
