// Writes a small v210 video flow at the media clock through the writer
// wrappers and serves what it wrote on /metrics. Part of docs/tutorial.md.
//
//   tutorial-writer <domain> [<metrics port>]

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>
#include <mxl-monitoring/http.h>
#include <mxl-monitoring/monitoring.h>
#include <mxl-monitoring/reader.h>
#include <mxl-monitoring/writer.h>

#define FLOW_ID "5fbec3b1-1b0f-417d-9059-8b94a47197ed"

// 48x8 pixels of v210 at 50 grains a second: small enough for any tmpfs.
static char const FlowDef[] =
    "{\"id\": \"" FLOW_ID "\", \"label\": \"tutorial\", \"description\": \"tutorial\", \"parents\": [],"
    " \"tags\": {\"urn:x-nmos:tag:grouphint/v1.0\": [\"tutorial:Video\"]},"
    " \"format\": \"urn:x-nmos:format:video\", \"media_type\": \"video/v210\","
    " \"grain_rate\": {\"numerator\": 50, \"denominator\": 1},"
    " \"frame_width\": 48, \"frame_height\": 8, \"interlace_mode\": \"progressive\", \"colorspace\": \"BT709\","
    " \"components\": [{\"name\": \"Y\", \"width\": 48, \"height\": 8, \"bit_depth\": 10},"
    " {\"name\": \"Cb\", \"width\": 24, \"height\": 8, \"bit_depth\": 10},"
    " {\"name\": \"Cr\", \"width\": 24, \"height\": 8, \"bit_depth\": 10}]}";

static volatile sig_atomic_t stopping = 0;

static void onSignal(int signal)
{
    (void)signal;
    stopping = 1;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <domain> [<metrics port>]\n", argv[0]);
        return 2;
    }
    char const* domain = argv[1];
    uint16_t const port = (uint16_t)(argc > 2 ? atoi(argv[2]) : 9101);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    mxlMonitoring monitoring;
    mxlMonitoringFlow flow;
    mxlMonitoringCreate(NULL, &monitoring);
    mxlMonitoringAddFlow(monitoring, FLOW_ID, MXL_MONITORING_ROLE_WRITER, &flow);
    mxlMonitoringHttp http;
    if (mxlMonitoringHttpStart(monitoring, NULL, port, &http) != MXL_STATUS_OK)
    {
        fprintf(stderr, "cannot serve metrics on port %u\n", port);
        return 1;
    }

    mxlInstance instance = mxlMonitoringCreateInstance(flow, domain, "");
    mxlFlowWriter writer;
    mxlFlowConfigInfo config;
    bool created;
    if (instance == NULL || mxlMonitoringCreateFlowWriter(flow, instance, FlowDef, "", &writer, &config, &created) != MXL_STATUS_OK)
    {
        fprintf(stderr, "cannot create flow %s in %s; curl localhost:%u/metrics says why\n", FLOW_ID, domain, port);
        return 1;
    }
    mxlRational const rate = config.common.grainRate;
    fprintf(stderr, "writing flow %s in %s, metrics on port %u\n", FLOW_ID, domain, port);

    uint64_t index = mxlGetCurrentIndex(&rate);
    while (!stopping)
    {
        mxlGrainInfo grain;
        uint8_t* payload;
        if (mxlMonitoringFlowWriterOpenGrain(flow, writer, index, &grain, &payload) == MXL_STATUS_OK)
        {
            // Mid grey: Y, Cb and Cr at code 512 in every 32-bit word.
            uint32_t const grey = 512u | (512u << 10) | (512u << 20);
            for (uint32_t offset = 0; offset + 4 <= grain.grainSize; offset += 4)
            {
                memcpy(payload + offset, &grey, 4);
            }
            // A grain counts as written on the commit that makes it whole.
            grain.validSlices = grain.totalSlices;
            mxlMonitoringFlowWriterCommitGrain(flow, writer, &grain);
        }
        mxlMonitoringPoll(monitoring);
        ++index;
        mxlSleepForNs(mxlGetNsUntilIndex(index, &rate));
    }

    mxlReleaseFlowWriter(instance, writer);
    mxlDestroyInstance(instance);
    mxlMonitoringHttpStop(http);
    mxlMonitoringDestroy(monitoring);
    return 0;
}
