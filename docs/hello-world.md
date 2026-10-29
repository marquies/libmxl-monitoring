# Hello world

A first program with libmxl-monitoring: a reader of one MXL video flow that
counts what it reads and serves the counts on `/metrics`. The whole program is
[examples/hello-world.c](../examples/hello-world.c), about 100 lines of C.

## What you need

- libmxl installed with its CMake package, such as under `/opt/libmxl` in
  `ghcr.io/qvest-digital/go-mxl-builder`
- vcpkg for picojson, as for the library itself (see [Build](../README.md#build))
- a directory on a tmpfs for the MXL domain, such as one under `/dev/shm`

## Build and run

The examples build with the library when it is the top-level project:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_PREFIX_PATH=/opt/libmxl
cmake --build build --target hello-world tutorial-writer
```

A reader needs a flow to read. `tutorial-writer`, from the
[tutorial](tutorial.md), writes one:

```sh
export LD_LIBRARY_PATH=/opt/libmxl/lib
mkdir -p /dev/shm/hello
build/examples/tutorial-writer /dev/shm/hello &
build/examples/hello-world /dev/shm/hello 5fbec3b1-1b0f-417d-9059-8b94a47197ed &
curl -s localhost:9100/metrics | grep -E '^mxl_reader_(flow_delivering|reads_total.*ok)'
```

```
mxl_reader_flow_delivering{flow="5fbec3b1-1b0f-417d-9059-8b94a47197ed"} 1
mxl_reader_reads_total{flow="5fbec3b1-1b0f-417d-9059-8b94a47197ed",result="ok"} 151
```

Three seconds of reading at 50 grains a second. Every other result, open
failures, latency histograms and the age of the last read are in the same
text.

## The program, step by step

One monitoring context per process, and one flow handle per flow it reads or
writes. A handle exists before the flow is opened, so a flow that cannot be
reached has somewhere to be counted:

```c
mxlMonitoring monitoring;
mxlMonitoringFlow flow;
mxlMonitoringCreate(NULL, &monitoring);
mxlMonitoringAddFlow(monitoring, flowId, MXL_MONITORING_ROLE_READER, &flow);
```

An HTTP endpoint of its own, for a process that has none; a process with an
HTTP server serves `mxlMonitoringPrometheusRender` from it instead:

```c
mxlMonitoringHttp http;
mxlMonitoringHttpStart(monitoring, NULL, port, &http);
```

The wrappers take the arguments of the libmxl call they are named after, plus
the flow handle, call libmxl, record what it returned, and return its status
unchanged:

```c
mxlInstance instance = mxlMonitoringCreateInstance(flow, domain, "");
mxlMonitoringCreateFlowReader(flow, instance, "", &reader);
mxlMonitoringFlowReaderGetGrain(flow, reader, index, 200000000, &grain, &payload);
```

What only the reader decides it records itself, such as the grains it jumped
over to get back to the head:

```c
mxlMonitoringRecordSkipped(flow, head - index);
```

Recording only increments counters. `mxlMonitoringPoll`, from one thread and
more often than once a second, turns them into events; hello world polls but
does not listen to the events, which the tutorial does.

`hello-world` ends when the flow goes away. The tutorial's reader opens it
again.

## In a project of your own

Inside a dmf-mxl/mxl checkout, or on its own against an installed libmxl:

```cmake
add_subdirectory(libmxl-monitoring)
target_link_libraries(my-reader PRIVATE mxl-monitoring-http)
```

`mxl-monitoring-http` brings `mxl-monitoring` and `mxl-monitoring-prometheus`
with it. Next: the [tutorial](tutorial.md).
