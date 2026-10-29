# libmxl-monitoring

A media function that reads or writes [MXL](https://github.com/dmf-mxl/mxl)
flows gets a status from every libmxl call and nothing more: no count of how
often a flow was out of reach, read too early or too late, or written without
its input in time, and no check that the content is what the flow declares.
AMWA's [IN-004](https://specs.amwa.tv/in-004/latest/Overview/) (Flow Connection
Phase 2 Requirements and Gaps, work in progress) asks for monitoring of MXL
writers, readers and replication: notification of these errors, counters of
them and of correct operation, and latency, over IS-12, OpenTelemetry or
ST 2138. This library provides it for readers and writers. For replication it
has the roles, which the Prometheus text renders; the OTLP exporter does not
export them yet.

A process records what happened to each flow it uses: whether the
domain and the flow could be reached, the outcome of every read or write, and
whether the content was what it should be. The library counts it, measures
latency against the grains' media timestamps, turns failures into rate-limited
events, and exports it all as Prometheus text or over OTLP.

It is a C API, implemented in C++20, built as static libraries against libmxl.
Linking it statically leaves `libmxl.so` the only library every process on a
domain has to load identically.

## What it records

| Outcome | Values |
| --- | --- |
| Access to the domain and flow | open, domain unavailable, flow not found, permission denied, flow invalid, other |
| Reads | ok, too early, too late, permission denied, error, invalid content |
| Writes | ok, input late, invalid input, permission denied, out of space, error |
| Content, for reads and writes | wrong format, wrong resolution, wrong rate, wrong size, missing, invalid, incomplete, illegal values |

Every libmxl status is classified into these by `mxlMonitoringClassify*`. A
too-early read counts once per index, and only once the index is a grace period
past its timestamp, since a reader at the head normally waits for the next
grain. A flow that disappears under an open reader or writer counts as a loss
of access, not as a failed read.

Each failure is a condition with three events: started, persisting (once a
minute while it recurs) and recovered. The events come from comparing the
counters between calls to `mxlMonitoringPoll`, so recording itself only
increments atomics. `DESIGN.md` gives the reasons behind these rules, and
[docs/architecture.md](docs/architecture.md) shows the parts on one page.

## Libraries

| Target | Header | Contents |
| --- | --- | --- |
| `mxl-monitoring` | `mxl-monitoring/monitoring.h` | flows, recording, snapshots, events |
| | `mxl-monitoring/content.h` | flow definitions, expectations, grain and sample checks |
| | `mxl-monitoring/reader.h` | libmxl reader calls that record their own outcome |
| | `mxl-monitoring/writer.h` | libmxl writer calls that record their own outcome |
| `mxl-monitoring-prometheus` | `mxl-monitoring/prometheus.h` | text exposition for `/metrics` |
| `mxl-monitoring-http` | `mxl-monitoring/http.h` | an endpoint serving `/metrics` and `/healthz`, for a process without an HTTP server |
| `mxl-monitoring-otlp` | `mxl-monitoring/otlp.h` | metrics and events pushed over OTLP/HTTP to an OpenTelemetry Collector; built with `-DMXL_MONITORING_OTLP=ON` |

In Prometheus text, readers are exported under `mxl_reader_*`, with the names
and label values `mxl-reader` serves; writers under `mxl_writer_*`;
replication under `mxl_gateway_source_*` and `mxl_gateway_target_*`. Over
OTLP, readers and writers are exported under the `mxl.*` names of
[DESIGN.md](DESIGN.md#opentelemetry), replication not yet.

## Use

[docs/hello-world.md](docs/hello-world.md) builds and runs a first reader, and
[docs/tutorial.md](docs/tutorial.md) a writer and a reader through access
failures, events and content checks; their programs are in
[examples/](examples/). [mxl-reader](https://github.com/marquies/dmf-mf-mxl-reader)
is a complete media function built on the library. In short:

```c
mxlMonitoring monitoring;
mxlMonitoringCreate(NULL, &monitoring);

mxlMonitoringFlow flow;
mxlMonitoringAddFlow(monitoring, flowId, MXL_MONITORING_ROLE_READER, &flow);

mxlInstance instance = mxlMonitoringCreateInstance(flow, domain, "");
mxlFlowReader reader;
if (mxlMonitoringCreateFlowReader(flow, instance, "", &reader) == MXL_STATUS_OK)
{
    mxlGrainInfo grain;
    uint8_t* payload;
    mxlMonitoringFlowReaderGetGrain(flow, reader, index, timeoutNs, &grain, &payload);
}

mxlMonitoringPoll(monitoring);   /* from one thread, more often than once a second */
mxlMonitoringPrometheusRender(monitoring, buffer, &size);
```

The OTLP exporter reads its configuration from the standard `OTEL_*`
environment variables, so a chart sets them and no code carries an address:

```c
mxlMonitoringOtlp otlp;
mxlMonitoringOtlpStart(monitoring, "mxl-reader", &otlp);  /* service.name unless OTEL_* set one */
/* ... */
mxlMonitoringOtlpStop(otlp);
```

```sh
OTEL_EXPORTER_OTLP_ENDPOINT=http://$NODE_IP:4318
OTEL_METRIC_EXPORT_INTERVAL=5000
OTEL_RESOURCE_ATTRIBUTES=dmf.media_function=clip-player-1
```

The wrappers take the arguments of the libmxl function they are named after and
return its status unchanged. A process that calls libmxl itself records with
`mxlMonitoringRecordRead`, `mxlMonitoringRecordWrite` and the other
`mxlMonitoringRecord*` functions instead.
`mxlMonitoringDescribe` gives the events of such a condition their text.

A flow handle is reported under its id and opens the libmxl flow of the same
id, unless `mxlMonitoringSetFlowTarget` points it at another: a reader slot
an NMOS controller connects keeps its name and its counts while the flow it
reads changes, and is `disconnected` while it reads none.

## Build

Inside a dmf-mxl/mxl checkout, after the `mxl` target is defined:

```cmake
add_subdirectory(libmxl-monitoring)
target_link_libraries(my-function PRIVATE mxl-monitoring mxl-monitoring-prometheus)
```

On its own, against an installed libmxl, with the vcpkg toolchain for picojson
and Catch2:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_PREFIX_PATH=/opt/libmxl
cmake --build build
LD_LIBRARY_PATH=/opt/libmxl/lib build/tests/mxl-monitoring-tests
```

The OTLP exporter needs opentelemetry-cpp with its OTLP/HTTP exporter, which
the manifest's `otlp` feature installs:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_PREFIX_PATH=/opt/libmxl \
  -DVCPKG_MANIFEST_FEATURES=otlp -DMXL_MONITORING_OTLP=ON
```

Inside a dmf-mxl/mxl checkout, vcpkg installs from that checkout's manifest,
which does not list opentelemetry-cpp; the consumer provides it.

`ghcr.io/qvest-digital/go-mxl-builder` carries libmxl under `/opt/libmxl` and
the vcpkg toolchain; CI builds both ways in it and runs the concurrency tests
under ThreadSanitizer as well. The reader tests need a writable `/dev/shm`.

## Roadmap

Released:

| Version | What it added |
| --- | --- |
| 0.1 | the core model, the reader wrappers, the content checks and the Prometheus text |
| 0.2 | a flow handle reported under a name of its own, so a reader slot keeps its counts while the flow it reads changes |
| 0.3 | the writer wrappers, and `/metrics` and `/healthz` for a process without an HTTP server |
| 0.4 | the OTLP/HTTP exporter: metrics and the events as log records |

Planned, in no fixed order:

- **BCP-008 mapping**: the statuses and counters of an AMWA BCP-008
  `NcReceiverMonitor` and `NcSenderMonitor`, worked out from a flow's
  snapshots: link, connection or transmission, synchronization, and stream or
  essence, with the lost, late and transmission error counters and their
  reset. It carries no NMOS dependency; an application hands the statuses to
  the IS-12 implementation of its Node.
- **Health evaluator**: thresholds and reporting delays from a configuration
  file, deciding a flow's health, which every exporter then exports.
- **Replication**: the source and target roles exist and the Prometheus text
  renders them with their read and write outcomes. Planned are the outcomes of
  the copy itself, which a gateway records around its own copy of a flow since
  no wrapper applies, and their export under `mxl.replication.*` over OTLP.
- **Go binding**: a `go/` module binding the C API through cgo, linking the
  static libraries and leaving libmxl to the Go side, so a gateway written in
  Go records and exports with the same names and rules.

Changes to libmxl that would let the library do more with less are candidates
to propose to [dmf-mxl/mxl](https://github.com/dmf-mxl/mxl), not
prerequisites: a status of its own for a full domain, which today the writer
wrappers infer from `errno` or the domain's free space; an observer hook reporting every read, write and open with its
status, which would make the wrappers unnecessary; and transfer counters in
libmxl-fabrics for replication.

## License

Apache-2.0, as MXL.
