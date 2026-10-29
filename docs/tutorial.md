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
status delivering         ok 102, too early 0, too late 0, invalid content 0
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
status open               ok 151, too early 1, too late 0, invalid content 0
event recovered  too_early, 1 occurrence(s)
status delivering         ok 402, too early 1, too late 0, invalid content 0
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
status flow_not_found     ok 501, too early 1, too late 0, invalid content 0
event started    flow_not_found, 1 occurrence(s): flow 5fbec3b1-1b0f-417d-9059-8b94a47197ed not found in the domain
event recovered  flow_not_found, 3 occurrence(s): flow 5fbec3b1-1b0f-417d-9059-8b94a47197ed not found in the domain
event recovered  flow_not_found, 1 occurrence(s): flow 5fbec3b1-1b0f-417d-9059-8b94a47197ed went away
status delivering         ok 502, too early 1, too late 0, invalid content 0
```

The writer releases its flow on exit and libmxl deletes it. The open reader
loses it, which counts as a loss of access rather than a failed read, and
each attempt to open it again counts as not found until the writer is back.

## 6. Checking the content

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
as invalid content, and the event says why:

```
event started    invalid_content, 1 occurrence(s): format differs from what was expected: 48x8 where 64x8 was expected
status delivering         ok 0, too early 0, too late 0, invalid content 151
```

In the Prometheus text these are `mxl_reader_reads_total{result="wrong_format"}`.
`checkLegal` checks every v210 code against the legal range as well; it is off
unless asked, since a test pattern may carry reserved codes on purpose.

## 7. Exporting

Both programs serve `/metrics` through `mxl-monitoring-http`. A process with
an HTTP server of its own serves `mxlMonitoringPrometheusRender` from it
instead. The OTLP exporter pushes the same counts, and the events as log
records, to a collector the standard `OTEL_*` variables name; it is a target
of its own, built with `-DMXL_MONITORING_OTLP=ON`, and not used by these
examples (see [Use](../README.md#use)). [data-model.md](data-model.md) lists
what each exporter sends.

## A complete media function

[mxl-reader](https://github.com/marquies/dmf-mf-mxl-reader) is what the
tutorial's reader becomes in production: it reads every grain or batch of
samples of each flow it is given through the reader wrappers, checks the
content against an expectation per flow, and serves the counts as `/metrics`,
`/status` as JSON and a live view of each flow's ring. As an NMOS Node it
gives each flow a Receiver a controller connects over IS-05, and moves the
flow handle with `mxlMonitoringSetFlowTarget`, so a Receiver keeps its name
and its counts while the flow it reads changes.
