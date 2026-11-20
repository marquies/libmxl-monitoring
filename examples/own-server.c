// A reader whose process serves HTTP of its own: its /metrics carries a
// metric of the process, then the Prometheus text of every flow. Links
// mxl-monitoring-prometheus, not mxl-monitoring-http. Part of
// docs/tutorial.md.
//
//   own-server <domain> <flow id> [<metrics port>]

#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/mxl.h>
#include <mxl/time.h>
#include <mxl-monitoring/monitoring.h>
#include <mxl-monitoring/prometheus.h>
#include <mxl-monitoring/reader.h>

static volatile sig_atomic_t stopping = 0;

static void onSignal(int signal)
{
    (void)signal;
    stopping = 1;
}

static mxlMonitoring monitoring;
static int listener;

static void sendAll(int client, char const* data, size_t size)
{
    while (size > 0)
    {
        ssize_t const n = send(client, data, size, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return;
        }
        data += n;
        size -= (size_t)n;
    }
}

// The process's own server, as small as it gets: one request per
// connection, read with one recv, GET /metrics only.
static void* serve(void* unused)
{
    (void)unused;
    unsigned long long requests = 0;
    int client;
    while ((client = accept(listener, NULL, NULL)) >= 0)
    {
        char request[1024];
        ssize_t const n = recv(client, request, sizeof request - 1, 0);
        if (n < 13 || memcmp(request, "GET /metrics ", 13) != 0)
        {
            static char const notFound[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, notFound, sizeof notFound - 1);
            close(client);
            continue;
        }
        ++requests;

        // The process's metric first, then the library's text after it.
        char own[256];
        size_t const ownSize = (size_t)snprintf(own, sizeof own,
            "# HELP own_server_requests_total Requests for /metrics this process served.\n"
            "# TYPE own_server_requests_total counter\n"
            "own_server_requests_total %llu\n",
            requests);
        // Recording between two calls can grow the text: a call that finds
        // the buffer too small sets the size it needs now, and the handler
        // renders again.
        size_t size = 0;
        mxlStatus status = mxlMonitoringPrometheusRender(monitoring, NULL, &size);
        char* body = NULL;
        for (size_t capacity = 0; status == MXL_ERR_INVALID_ARG && size > capacity;)
        {
            capacity = size;
            char* const grown = realloc(body, ownSize + capacity);
            if (grown == NULL)
            {
                break;
            }
            body = grown;
            status = mxlMonitoringPrometheusRender(monitoring, body + ownSize, &size);
        }
        if (status != MXL_STATUS_OK)
        {
            static char const failed[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, failed, sizeof failed - 1);
            free(body);
            close(client);
            continue;
        }
        memcpy(body, own, ownSize);

        char header[160];
        int const headerSize = snprintf(header, sizeof header,
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
            ownSize + size - 1);
        sendAll(client, header, (size_t)headerSize);
        sendAll(client, body, ownSize + size - 1);
        free(body);
        close(client);
    }
    return NULL;
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
    uint16_t const port = (uint16_t)(argc > 3 ? atoi(argv[3]) : 9103);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    mxlMonitoringFlow flow;
    mxlMonitoringCreate(NULL, &monitoring);
    mxlMonitoringAddFlow(monitoring, flowId, MXL_MONITORING_ROLE_READER, &flow);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    int const yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {htonl(INADDR_ANY)}};
    if (bind(listener, (struct sockaddr*)&address, sizeof address) != 0 || listen(listener, 8) != 0)
    {
        fprintf(stderr, "cannot serve metrics on port %u\n", port);
        return 1;
    }
    pthread_t server;
    pthread_create(&server, NULL, serve, NULL);

    mxlInstance instance = mxlMonitoringCreateInstance(flow, domain, "");
    mxlFlowReader reader;
    if (instance == NULL || mxlMonitoringCreateFlowReader(flow, instance, "", &reader) != MXL_STATUS_OK)
    {
        fprintf(stderr, "cannot open flow %s in %s\n", flowId, domain);
        return 1;
    }
    mxlFlowInfo info;
    mxlFlowReaderGetInfo(reader, &info);
    mxlRational const rate = info.config.common.grainRate;

    // The read loop of hello-world.c.
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
            uint64_t const head = mxlGetCurrentIndex(&rate);
            mxlMonitoringRecordSkipped(flow, head - index);
            index = head;
        }
        else if (status != MXL_ERR_OUT_OF_RANGE_TOO_EARLY && status != MXL_ERR_TIMEOUT)
        {
            break;
        }
        mxlMonitoringPoll(monitoring);
    }

    mxlReleaseFlowReader(instance, reader);
    mxlDestroyInstance(instance);
    // Wakes the accept of the server thread, which then returns.
    shutdown(listener, SHUT_RDWR);
    pthread_join(server, NULL);
    close(listener);
    mxlMonitoringDestroy(monitoring);
    return 0;
}
