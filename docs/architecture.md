# Architecture

How libmxl-monitoring is put together, in one page. [README.md](../README.md)
covers use and build, [DESIGN.md](../DESIGN.md) the reasoning behind every
rule; the headers in [include/mxl-monitoring/](../include/mxl-monitoring/)
are the reference for every call.

Diagrams: [components](components.puml),
[record-to-export sequence](record-to-export.puml).
[data-model.md](data-model.md) details what Prometheus and the OTel
Collector receive.

## The idea in one paragraph

A process that reads or writes MXL flows records what happened to each flow:
could it be reached, did the read or write succeed, was the content right.
The library counts those outcomes into per-flow atomics, turns changes into
rate-limited events, and renders everything for Prometheus or OTLP. Recording
never blocks, so it may run inside a media loop; everything with history or
I/O runs elsewhere.

## Components

| Component | Library | Headers / sources |
| --- | --- | --- |
| Core model: flows, counters, snapshots, events | `mxl-monitoring` | `monitoring.h`, `Monitoring.cpp`, `Flow.hpp`, `Events.cpp` |
| Content checks against the flow definition | `mxl-monitoring` | `content.h`, `Content.cpp` |
| libmxl wrappers that record their own outcome | `mxl-monitoring` | `reader.h`/`Reader.cpp`, `writer.h`/`Writer.cpp` |
| Prometheus text exposition | `mxl-monitoring-prometheus` | `prometheus.h`, `Prometheus.cpp` |
| Minimal `/metrics` + `/healthz` endpoint | `mxl-monitoring-http` | `http.h`, `Http.cpp` |
| OTLP/HTTP push exporter (optional) | `mxl-monitoring-otlp` | `otlp.h`, `Otlp.cpp` |

All targets are static libraries with a C API over a C++20 implementation.
The exporters depend on the core, never the other way around; an exporter
with heavy dependencies (OTLP) is its own target so only processes that link
it carry them.

## Core model

- One `mxlMonitoring` context per process, one `mxlMonitoringFlow` handle per
  flow the process reads or writes.
- A handle exists before the flow is opened, so failures to reach the flow
  have somewhere to be counted. `mxlMonitoringSetFlowTarget` can point it at
  another libmxl flow while keeping its name and counts.
- Per flow: access state, read/write/content counters, skipped and stall
  counts, three latency histograms (arrival, commit, input), the last error,
  and whether the flow is delivering.

## Data flow

```
media loop            polling thread              exporter threads
----------            --------------              ----------------
wrappers or           mxlMonitoringPoll:          Prometheus render /
Record* calls   --->  diff counters vs      --->  HTTP endpoint /
(atomics only)        last poll, emit             OTLP push
                      started / persisting /      (snapshots + events)
                      recovered events
```

1. **Record** (hot path): wrappers around libmxl calls, or explicit
   `mxlMonitoringRecord*` calls, classify each `mxlStatus` into an access,
   read or write outcome and increment relaxed atomics of their own flow.
   No lock, no allocation, no event state.
2. **Poll** (one thread): `mxlMonitoringPoll` compares each flow's counters
   with those of the previous poll and turns changes into condition events,
   `started`, `persisting` (rate-limited) and `recovered`, delivered to
   subscribers on the polling thread.
3. **Export** (own schedule): exporters call `mxlMonitoringSnapshot` for a
   consistent copy of every flow's counts. Prometheus renders pull-style
   text; the HTTP endpoint serves it for processes without a server; OTLP
   pushes metrics on its interval and forwards events as log records.

## Threading contract

| Who | Where | May block? |
| --- | --- | --- |
| Recording | one thread per flow, inside the media loop | never |
| `mxlMonitoringPoll` + event subscribers | one thread, more often than the recover delay | yes (holds the context lock) |
| Exporters | their own threads | yes (read snapshots) |

This split is the central design decision: the hot path carries no event
state, so exporters and subscribers can never stall a read or write loop.
