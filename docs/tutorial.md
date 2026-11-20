# Tutorial

A writer and a reader of one MXL flow, both monitored, and what each reports
when the flow breaks: [examples/tutorial-writer.c](../examples/tutorial-writer.c)
and [examples/tutorial-reader.c](../examples/tutorial-reader.c). Start with
[Hello world](hello-world.md) for the build; the commands below assume its
`build` directory and `LD_LIBRARY_PATH`.

## 1. A writer that counts its writes

`tutorial-writer` creates a 48x8 v210 flow at 50 grains a second and writes a
grey grain at every index, through the writer wrappers:

```c
mxlMonitoringAddFlow(monitoring, FLOW_ID, MXL_MONITORING_ROLE_WRITER, &flow);
mxlInstance instance = mxlMonitoringCreateInstance(flow, domain, "");
mxlMonitoringCreateFlowWriter(flow, instance, FlowDef, "", &writer, &config, &created);

mxlMonitoringFlowWriterOpenGrain(flow, writer, index, &grain, &payload);
/* fill the payload */
grain.validSlices = grain.totalSlices;
mxlMonitoringFlowWriterCommitGrain(flow, writer, &grain);
```

A grain counts as written once, on the commit that makes all its slices
valid; a grain committed with the invalid flag does not count, and the writer
records why it wrote one with `mxlMonitoringRecordWriteOutcome`.

```sh
mkdir -p /dev/shm/tutorial
build/examples/tutorial-writer /dev/shm/tutorial &
curl -s localhost:9101/metrics | grep -E '^mxl_writer_(flow_delivering|writes_total.*ok)'
```

```
mxl_writer_flow_delivering{flow="5fbec3b1-1b0f-417d-9059-8b94a47197ed"} 1
mxl_writer_writes_total{flow="5fbec3b1-1b0f-417d-9059-8b94a47197ed",result="ok"} 201
```

## 2. A reader that keeps going

`tutorial-reader` is hello world with four additions. The first: when the
flow goes away it opens it again, once a second, and every attempt that fails
counts by the reason libmxl gave:

```c
while (!stopping)
{
    mxlInstance instance = mxlMonitoringCreateInstance(flow, domain, "");
    if (instance != NULL && mxlMonitoringCreateFlowReader(flow, instance, "", &reader) == MXL_STATUS_OK)
    {
        readGrains(monitoring, flow, reader, rate);
        mxlReleaseFlowReader(instance, reader);
    }
    /* destroy the instance, poll and wait a second */
}
```

In a second terminal, as it runs in the foreground:

```sh
build/examples/tutorial-reader /dev/shm/tutorial 5fbec3b1-1b0f-417d-9059-8b94a47197ed 9102
```

## 3. Where the flow stands

The second: a status line every second, from a snapshot, a consistent copy of
every count of a flow:

```c
mxlMonitoringFlowSnapshot snapshot = {.version = 1, .size = sizeof snapshot};
size_t count = 1;
mxlMonitoringSnapshot(monitoring, &snapshot, &count);
```

```
status delivering         ok 102, too early 0, too late 0, invalid essence 0
```

`delivering` is true while data moved within the last second; otherwise the
line names the access state, such as `open` or `flow_not_found`.

## 4. Events

The third: a subscriber. `mxlMonitoringPoll` compares the counts with those
of the previous poll and calls every subscriber, on the polling thread, once
a condition starts, once a minute while it persists, and once it is over:

```c
mxlMonitoringSubscribe(monitoring, onEvent, NULL);
```

A subscriber must not call back into the context. A slow one holds up
`mxlMonitoringRemoveFlow` and `mxlMonitoringSubscribe`, though never the
recording.

## 5. Breaking the flow

Stop the writer for four seconds, then let it go on:

```sh
pkill -STOP tutorial-writer; sleep 4; pkill -CONT tutorial-writer
```

```
event started    too_early, 1 occurrence(s)
status open               ok 151, too early 1, too late 0, invalid essence 0
event recovered  too_early, 1 occurrence(s)
status delivering         ok 402, too early 1, too late 0, invalid essence 0
```

A reader at the head waits for the next grain, so a too-early read counts
only once its index is a grace period past its timestamp, and once per index.
The flow stays `open` but is no longer delivering.

Stop the writer for good, and start it again:

```sh
pkill tutorial-writer
build/examples/tutorial-writer /dev/shm/tutorial &
```

```
event started    flow_not_found, 1 occurrence(s): flow 5fbec3b1-1b0f-417d-9059-8b94a47197ed went away
status flow_not_found     ok 501, too early 1, too late 0, invalid essence 0
event started    flow_not_found, 1 occurrence(s): flow 5fbec3b1-1b0f-417d-9059-8b94a47197ed not found in the domain
event recovered  flow_not_found, 3 occurrence(s): flow 5fbec3b1-1b0f-417d-9059-8b94a47197ed not found in the domain
event recovered  flow_not_found, 1 occurrence(s): flow 5fbec3b1-1b0f-417d-9059-8b94a47197ed went away
status delivering         ok 502, too early 1, too late 0, invalid essence 0
```

The writer releases its flow on exit and libmxl deletes it. The open reader
loses it, which counts as a loss of access rather than a failed read, and
each attempt to open it again counts as not found until the writer is back.

## 6. Checking the essence

The fourth: an expectation. The reader wrappers check every grain against it
and against the flow's own definition:

```c
mxlMonitoringExpectation const expected = {
    .version = 1, .size = sizeof expected,
    .mediaType = "video/v210", .width = 48, .height = 8, .rate = {50, 1},
    .checkLegal = true,
};
mxlMonitoringSetExpectation(flow, &expected);
```

Change `.width` to 64, build and run the reader again: every grain now counts
as invalid essence, and the event says why:

```
event started    invalid_essence, 1 occurrence(s): format differs from what was expected: 48x8 where 64x8 was expected
status delivering         ok 0, too early 0, too late 0, invalid essence 151
```

In the Prometheus text these are `mxl_reader_reads_total{result="wrong_format"}`.
`checkLegal` checks every v210 code against the legal range as well; it is off
unless asked, since a test pattern may carry reserved codes on purpose.

## 7. Exporting

Both programs serve `/metrics` through `mxl-monitoring-http`. The OTLP
exporter pushes the same counts, and the events as log records, to a
collector the standard `OTEL_*` variables name; it is a target of its own,
built with `-DMXL_MONITORING_OTLP=ON`, and not used by these programs (see
[Use](../README.md#use)). [data-model.md](data-model.md) lists what each
exporter sends.

## 8. In a process that already exports

A process that serves metrics or runs OpenTelemetry of its own adds the
library to what it has. Each program here reads the flow `tutorial-writer`
writes, with the read loop of hello world. The two in C++ build only with
`-DMXL_MONITORING_OTLP=ON`, which brings opentelemetry-cpp.

### A metrics endpoint of its own

[examples/own-server.c](../examples/own-server.c) serves HTTP itself and
links `mxl-monitoring-prometheus` instead of `mxl-monitoring-http`. Its
handler writes the metrics of the process, then the library's text after
them:

```c
size_t size = 0;
mxlStatus status = mxlMonitoringPrometheusRender(monitoring, NULL, &size); /* sizes the buffer */
char* body = NULL;
for (size_t capacity = 0; status == MXL_ERR_INVALID_ARG && size > capacity;)
{
    capacity = size;
    body = realloc(body, ownSize + capacity); /* the example checks it */
    status = mxlMonitoringPrometheusRender(monitoring, body + ownSize, &size);
}
memcpy(body, own, ownSize);
```

```sh
build/examples/own-server /dev/shm/tutorial 5fbec3b1-1b0f-417d-9059-8b94a47197ed 9103 &
curl -s localhost:9103/metrics | grep -E '^(own_server|mxl_reader_reads_total.*ok)'
```

```
own_server_requests_total 1
mxl_reader_reads_total{flow="5fbec3b1-1b0f-417d-9059-8b94a47197ed",result="ok"} 150
```

The render may run on the server's thread while the reader records.
Recording between the two calls can grow the text; the second call then
returns `MXL_ERR_INVALID_ARG` with the size it needs now, and the handler
renders again.

### An OpenTelemetry setup of its own

[examples/beside-otlp.cpp](../examples/beside-otlp.cpp) sets up a
MeterProvider of its own with an OTLP exporter, makes it the global one and
counts grains with it. The library's exporter runs beside it:

```cpp
opentelemetry::metrics::Provider::SetMeterProvider(std::shared_ptr<opentelemetry::metrics::MeterProvider>{provider});
mxlMonitoringOtlpStart(monitoring, "beside-otlp", &exporter);
```

The library builds an OTLP metric exporter and a LoggerProvider of its own,
and neither sets nor reads the global providers: its metrics do not pass
through the process's MeterProvider, and the process's configuration stays
as it is. Both read the same `OTEL_*` variables, so both push to the same
collector; with `OTEL_SERVICE_NAME` set, both carry the same `service.name`.
The library's exporter takes `OTEL_METRIC_EXPORT_INTERVAL` as given, while
opentelemetry-cpp's periodic reader falls back to 60 s unless
`OTEL_METRIC_EXPORT_TIMEOUT`, 30 s by default, is below it; a process that
exports every 30 s or more often sets both.

`mxl-monitoring-otlp` is a static library; the executable links it and
opentelemetry-cpp together, so the process builds against the
opentelemetry-cpp the library was built with, here the version vcpkg
installs for the `otlp` feature, and then holds one copy of it.

### Instruments of its own

[examples/own-instruments.cpp](../examples/own-instruments.cpp) uses no
exporter of the library, only the core. Its observable instruments take a
snapshot in their callbacks, and a subscriber hands every event to its
logger:

```cpp
reads->AddCallback(observeReads, monitoring); /* mxlMonitoringSnapshot, a point per outcome */
mxlMonitoringSubscribe(monitoring, onEvent, logger.get());
```

The callbacks run on the SDK's reader thread; the subscriber runs on the
polling thread, as in section 4, and hands the record to a batch processor.
The names then live in the process: `own-instruments` uses those of
[data-model.md](data-model.md) for `mxl.reader.reads` and
`mxl.reader.delivering`, with the same attributes, and every other metric, and any the library adds
later, is missing until the process maps it too.

### Go

The Go binding renders the same text with `PrometheusText`, which a handler
of the process serves beside its own metrics;
[go/example_test.go](../go/example_test.go) does so with `net/http` alone:

```go
mux.HandleFunc("/metrics", func(w http.ResponseWriter, _ *http.Request) {
	text, err := m.PrometheusText()
	/* ... */
	fmt.Fprintf(w, "# TYPE own_server_requests_total counter\nown_server_requests_total %d\n", requests.Add(1))
	fmt.Fprint(w, text)
})
```

`Snapshot` gives the counts to instruments of the process's own. The binding
has no event subscription: the events leave a Go process only as the log
records of `go/otlp`.

## A complete media function

[mxl-reader](https://github.com/marquies/dmf-mf-mxl-reader) is what the
tutorial's reader becomes in production: it reads every grain or batch of
samples of each flow it is given through the reader wrappers, checks the
essence against an expectation per flow, and serves the counts as `/metrics`,
`/status` as JSON and a live view of each flow's ring. As an NMOS Node it
gives each flow a Receiver a controller connects over IS-05, and moves the
flow handle with `mxlMonitoringSetFlowTarget`, so a Receiver keeps its name
and its counts while the flow it reads changes.
