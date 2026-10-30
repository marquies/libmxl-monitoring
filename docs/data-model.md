# Data model of exported monitoring data

What leaves the process, and in which shape. Two structures are the source of
everything external services receive, both defined in
[monitoring.h](../include/mxl-monitoring/monitoring.h):

| Structure | Produced by | Consumed by |
| --- | --- | --- |
| `mxlMonitoringFlowSnapshot`: one per flow, a consistent copy of all its counts | `mxlMonitoringSnapshot` | Prometheus text rendering, OTLP metric points |
| `mxlMonitoringEvent`: one per condition change (started / persisting / recovered) | `mxlMonitoringPoll` | event subscribers, OTLP log records |

Diagram: [data-model.puml](data-model.puml). Rationale for names and
values: [DESIGN.md](../DESIGN.md#exporters).

## Source structures

**Snapshot** (per flow): identity (`flowId`, `target`, `role`), format
(`unit`, `format`, `rate`, `mediaType`), access state and failure counts,
`opens` and `losses`, read/write outcome counters, content issue counters,
`skipped`, `stalls`, the last error (status, index, time, text),
`formatMismatch`, three latency histograms (arrival, commit, input; 18 fixed
buckets, 1 ms to 81.92 s), `lastIndex`, `ageNs` and `delivering`.

**Event** (per condition change): `timeNs`, `flowId`, `role`, condition
(`kind` + access/read/write outcome + content issue), `phase`, last libmxl
`status`, last affected `index`, `occurrences` since the previous event,
`total` and `durationNs` since started, a one-line `detail` text, and the
flow's `format` and `unit`.

## Prometheus (pull: text exposition)

Rendered from snapshots by `mxl-monitoring-prometheus`
([src/Prometheus.cpp](../src/Prometheus.cpp)). Each role gets its own prefix: `mxl_reader`,
`mxl_writer`, `mxl_gateway_source`, `mxl_gateway_target`. Every series
carries a `flow` label with the flow's id.

| Metric | Type | Extra labels | Snapshot source |
| --- | --- | --- | --- |
| `<prefix>_flow_state` | gauge (0/1) | `state`: access names | `access` |
| `<prefix>_flow_delivering` | gauge (0/1) | | `delivering` |
| `<prefix>_reads_total` | counter | `result`: `ok`, `too_early`, `too_late`, `permission_error`, `other_error`, plus content issues as own results | `reads` + `content` |
| `<prefix>_writes_total` | counter | `result`: write outcomes | `writes` |
| `<prefix>_invalid_input_total` | counter | `issue`: content issues | `content` |
| `<prefix>_open_failures_total` | counter | `reason`: `domain`, `not_found`, `permission`, `other` | `accessFailures` |
| `<prefix>_skipped_total`, `<prefix>_stalls_total` | counter | | `skipped`, `stalls` |
| `<prefix>_opens_total`, `<prefix>_lost_total` | counter | | `opens`, `losses` |
| `<prefix>_age_seconds` | gauge | | `ageNs` |
| `<prefix>_arrival_seconds`, `<prefix>_commit_seconds`, `<prefix>_input_seconds` | histogram | `le` buckets | the three histograms |

Reads and arrival are rendered for reader-side roles, writes and input for
writer-side ones. Events are not exported here: Prometheus is pull-only, and
counters already carry the rates.

## OTLP (push: metrics + log records)

Pushed by `mxl-monitoring-otlp` ([src/Otlp.cpp](../src/Otlp.cpp)). Every point and
record carries `mxl.flow.id`, and once the flow is open `mxl.flow.media_type`
and `mxl.flow.unit`. Replication roles are not exported yet.

### Metric points (from snapshots, cumulative, every export interval)

| Metric | Type | Extra attributes |
| --- | --- | --- |
| `mxl.flow.state` | gauge | `mxl.flow.state`: access names |
| `mxl.flow.open.failures` | counter | `mxl.open.failure` |
| `mxl.flow.opens`, `mxl.flow.reopens` | counter | |
| `mxl.reader.reads` | counter | `mxl.read.result`, with `mxl.content.issue` for `invalid_content` |
| `mxl.reader.skipped`, `mxl.reader.stalls` | counter | |
| `mxl.reader.delivering`, `mxl.reader.age` | gauge | |
| `mxl.reader.arrival.latency`, `mxl.reader.commit.latency` | histogram | |
| `mxl.writer.writes` | counter | `mxl.write.result`, with `mxl.content.issue` for `invalid_input` |
| `mxl.writer.delivering`, `mxl.writer.head` | gauge | |
| `mxl.writer.commit.latency`, `mxl.writer.input.latency` | histogram | |

### Log records (one per event)

| Field | Value |
| --- | --- |
| event name | `mxl.flow.open_failed` / `open_failing` / `opened`; `mxl.reader.read_failed` / `read_recovered`; `mxl.writer.write_failed` / `write_recovered` |
| severity | WARN for `too_early`, `too_late`, `input_late`; ERROR for other failures; INFO for a recovery |
| body | the event's one-sentence `detail` |
| attributes | the outcome (`mxl.open.failure` / `mxl.read.result` / `mxl.write.result`), `mxl.content.issue`, `mxl.event.phase`, `mxl.status`, `mxl.grain.index`, `mxl.occurrences`, `mxl.total`, `mxl.duration` |

A record is self-contained: a subscriber needs nothing but the event to see
what failed, on which flow, how often and for how long.
