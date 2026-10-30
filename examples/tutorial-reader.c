// The reader of docs/tutorial.md: hello-world.c with an expectation the
// content of every grain is checked against, the events printed as they
// come, a status line every second, and the flow opened again whenever it
// goes away.
//
//   tutorial-reader <domain> <flow id> [<metrics port>]

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/mxl.h>
#include <mxl/time.h>
#include <mxl-monitoring/content.h>
#include <mxl-monitoring/http.h>
#include <mxl-monitoring/monitoring.h>
#include <mxl-monitoring/reader.h>

static volatile sig_atomic_t stopping = 0;

static void onSignal(int signal)
{
    (void)signal;
    stopping = 1;
}

// Called on the thread that polls, once a condition starts, once a minute
// while it persists, and once it is over. The detail is text libmxl or a
// check gave, empty where neither had more to say than the outcome.
static void onEvent(mxlMonitoringEvent const* event, void* user)
{
    (void)user;
    static char const* const phases[] = {"started", "persisting", "recovered"};
    char const* const condition = event->kind == MXL_MONITORING_CONDITION_ACCESS ? mxlMonitoringAccessName(event->access)
                                : event->kind == MXL_MONITORING_CONDITION_READ   ? mxlMonitoringReadName(event->read)
                                                                                 : mxlMonitoringWriteName(event->write);
    printf("event %-10s %s, %llu occurrence(s)%s%s\n", phases[event->phase], condition, (unsigned long long)event->total,
        event->detail[0] != '\0' ? ": " : "", event->detail);
    fflush(stdout);
}

static uint64_t nowNs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

// Prints where the flow stands, from a consistent copy of its counts.
static void printStatus(mxlMonitoring monitoring)
{
    mxlMonitoringFlowSnapshot snapshot = {.version = 1, .size = sizeof snapshot};
    size_t count = 1;
    if (mxlMonitoringSnapshot(monitoring, &snapshot, &count) != MXL_STATUS_OK || count == 0)
    {
        return;
    }
    printf("status %-18s ok %llu, too early %llu, too late %llu, invalid content %llu\n",
        snapshot.delivering ? "delivering" : mxlMonitoringAccessName(snapshot.access),
        (unsigned long long)snapshot.reads[MXL_MONITORING_READ_OK], (unsigned long long)snapshot.reads[MXL_MONITORING_READ_TOO_EARLY],
        (unsigned long long)snapshot.reads[MXL_MONITORING_READ_TOO_LATE],
        (unsigned long long)snapshot.reads[MXL_MONITORING_READ_INVALID_CONTENT]);
    fflush(stdout);
}

// Reads grains at the media clock until the flow fails for good.
static void readGrains(mxlMonitoring monitoring, mxlMonitoringFlow flow, mxlFlowReader reader, mxlRational rate)
{
    uint64_t index = mxlGetCurrentIndex(&rate);
    uint64_t nextStatus = nowNs();
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
            uint64_t const head = mxlGetCurrentIndex(&rate);
            mxlMonitoringRecordSkipped(flow, head - index);
            index = head;
        }
        else if (status != MXL_ERR_OUT_OF_RANGE_TOO_EARLY && status != MXL_ERR_TIMEOUT)
        {
            return; // the flow went away; the caller opens it again
        }
        mxlMonitoringPoll(monitoring);
        if (nowNs() >= nextStatus)
        {
            printStatus(monitoring);
            nextStatus += 1000000000u;
        }
    }
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

    mxlMonitoring monitoring;
    mxlMonitoringFlow flow;
    mxlMonitoringCreate(NULL, &monitoring);
    mxlMonitoringAddFlow(monitoring, flowId, MXL_MONITORING_ROLE_READER, &flow);
    mxlMonitoringSubscribe(monitoring, onEvent, NULL);

    // What the flow must carry. A flow declaring anything else counts its
    // grains as wrong_format; checkLegal checks every v210 code as well.
    mxlMonitoringExpectation const expected = {
        .version = 1,
        .size = sizeof expected,
        .mediaType = "video/v210",
        .width = 48,
        .height = 8,
        .rate = {50, 1},
        .checkLegal = true,
    };
    mxlMonitoringSetExpectation(flow, &expected);

    mxlMonitoringHttp http;
    if (mxlMonitoringHttpStart(monitoring, NULL, port, &http) != MXL_STATUS_OK)
    {
        fprintf(stderr, "cannot serve metrics on port %u\n", port);
        return 1;
    }

    // Open, read, and open again once the flow comes back: every attempt
    // that fails is counted by the reason libmxl gave.
    while (!stopping)
    {
        mxlInstance instance = mxlMonitoringCreateInstance(flow, domain, "");
        mxlFlowReader reader;
        if (instance != NULL && mxlMonitoringCreateFlowReader(flow, instance, "", &reader) == MXL_STATUS_OK)
        {
            mxlFlowInfo info;
            mxlFlowReaderGetInfo(reader, &info);
            readGrains(monitoring, flow, reader, info.config.common.grainRate);
            mxlReleaseFlowReader(instance, reader);
        }
        if (instance != NULL)
        {
            mxlDestroyInstance(instance);
        }
        for (int i = 0; i < 10 && !stopping; ++i)
        {
            mxlMonitoringPoll(monitoring);
            mxlSleepForNs(100000000);
        }
        printStatus(monitoring);
    }

    mxlMonitoringHttpStop(http);
    mxlMonitoringUnsubscribe(monitoring, onEvent, NULL);
    mxlMonitoringDestroy(monitoring);
    return 0;
}
