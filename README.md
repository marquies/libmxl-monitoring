# libmxl-monitoring

Monitoring for processes that read or write [MXL](https://github.com/dmf-mxl/mxl)
flows. A process records what happened to each flow it uses: whether the
domain and the flow could be reached, the outcome of every read or write, and
whether the content was what it should be. The library counts it, measures
latency against the grains' media timestamps, turns failures into rate-limited
events, and renders it all for Prometheus.

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
increments atomics. `DESIGN.md` gives the reasons behind these rules.

## Libraries

| Target | Header | Contents |
| --- | --- | --- |
| `mxl-monitoring` | `mxl-monitoring/monitoring.h` | flows, recording, snapshots, events |
| | `mxl-monitoring/content.h` | flow definitions, expectations, grain and sample checks |
| | `mxl-monitoring/reader.h` | libmxl reader calls that record their own outcome |
| | `mxl-monitoring/writer.h` | libmxl writer calls that record their own outcome |
| `mxl-monitoring-prometheus` | `mxl-monitoring/prometheus.h` | text exposition for `/metrics` |
| `mxl-monitoring-http` | `mxl-monitoring/http.h` | an endpoint serving `/metrics` and `/healthz`, for a process without an HTTP server |

Readers are exported under `mxl_reader_*`, with the names and label values
`mxl-reader` serves; writers under `mxl_writer_*`; replication under
`mxl_gateway_source_*` and `mxl_gateway_target_*`.

## Use

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

`ghcr.io/qvest-digital/go-mxl-builder` carries libmxl under `/opt/libmxl` and
the vcpkg toolchain; CI builds both ways in it and runs the concurrency tests
under ThreadSanitizer as well. The reader tests need a writable `/dev/shm`.

## License

Apache-2.0, as MXL.
