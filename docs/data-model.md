# Data model of exported monitoring data

What leaves the process, and in which shape. Two structures are the source of
everything external services receive, both defined in
[monitoring.h](../include/mxl-monitoring/monitoring.h):

| Structure | Produced by | Consumed by |
| --- | --- | --- |
| `mxlMonitoringFlowSnapshot`: one per flow, a consistent copy of all its counts | `mxlMonitoringSnapshot` | Prometheus text rendering, OTLP metric points |
| `mxlMonitoringEvent`: one per condition change (started / persisting / recovered) | `mxlMonitoringPoll` | event subscribers, OTLP log records |

The NMOS export reads the same snapshots: two of one flow, taken at
different times, give a monitor's statuses and counters (see
[NMOS](#nmos-is-12-bcp-008-01-and-bcp-008-02)).

Diagram: [data-model.puml](data-model.puml). Rationale for names and
values: [DESIGN.md](../DESIGN.md#exporters).

## Source structures

**Snapshot** (per flow): identity (`flowId`, `target`, `role`), format
(`unit`, `format`, `rate`, `mediaType`), access state and failure counts,
`opens` and `accessLosses`, read/write outcome counters, essence issue counters,
`skipped`, `stalls`, the last error (status, index, time, text),
`formatMismatch`, three latency histograms (arrival, commit, input; 18 fixed
buckets, 1 ms to 81.92 s), `lastIndex`, `ageNs` and `delivering`.

**Event** (per condition change): `timeNs`, `flowId`, `role`, condition
(`kind` + access/read/write outcome + essence issue), `phase`, last libmxl
`status`, last affected `index`, `occurrences` since the previous event,
`total` and `durationNs` since started, a one-line `detail` text, and the
flow's `format` and `unit`.

## Prometheus (pull: text exposition)

Rendered from snapshots by `mxl-monitoring-prometheus`
([src/Prometheus.cpp](../src/Prometheus.cpp)). Each role gets its own prefix: `mxl_reader`,
`mxl_writer`, `mxl_replication_source`, `mxl_replication_target`. Every series
carries a `flow` label with the flow's id.

| Metric | Type | Extra labels | Snapshot source |
| --- | --- | --- | --- |
| `<prefix>_flow_state` | gauge (0/1) | `state`: access names | `access` |
| `<prefix>_flow_delivering` | gauge (0/1) | | `delivering` |
| `<prefix>_reads_total` | counter | `result`: reader: `ok`, `too_early`, `too_late`, `permission_error`, `other_error`, plus essence issues as own results; source: the read outcome names | `reads` + `essence` |
| `<prefix>_writes_total` | counter | `result`: write outcome names | `writes` |
| `<prefix>_invalid_essence_total` | counter | `issue`: essence issues (source) | `essence` |
| `<prefix>_invalid_input_total` | counter | `issue`: essence issues | `essence` |
| `<prefix>_open_failures_total` | counter | `reason`: reader: `domain`, `not_found`, `permission`, `other`; others: `domain_unavailable`, `flow_not_found`, `permission_denied`, `flow_invalid`, `other` | `accessFailures` |
| `<prefix>_skipped_total`, `<prefix>_stalls_total` | counter | | `skipped`, `stalls` |
| `<prefix>_opens_total`, `<prefix>_access_losses_total` | counter | | `opens`, `accessLosses` |
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
| `mxl.flow.opens`, `mxl.flow.access.losses` | counter | |
| `mxl.reader.reads` | counter | `mxl.read.result`, with `mxl.essence.issue` for `invalid_essence` |
| `mxl.reader.skipped`, `mxl.reader.stalls` | counter | |
| `mxl.reader.delivering`, `mxl.reader.age` | gauge | |
| `mxl.reader.arrival.latency`, `mxl.reader.commit.latency` | histogram | |
| `mxl.writer.writes` | counter | `mxl.write.result`, with `mxl.essence.issue` for `invalid_input` |
| `mxl.writer.delivering`, `mxl.writer.head` | gauge | |
| `mxl.writer.commit.latency`, `mxl.writer.input.latency` | histogram | |

### Log records (one per event)

| Field | Value |
| --- | --- |
| event name | `mxl.flow.open_failed` / `open_failing` / `opened`; `mxl.reader.read_failed` / `read_recovered`; `mxl.writer.write_failed` / `write_recovered` |
| severity | WARN for `too_early`, `too_late`, `input_late`; ERROR for other failures; INFO for a recovery |
| body | the event's one-sentence `detail` |
| attributes | the outcome (`mxl.open.failure` / `mxl.read.result` / `mxl.write.result`), `mxl.essence.issue`, `mxl.event.phase`, `mxl.status`, `mxl.grain.index`, `mxl.occurrences`, `mxl.total`, `mxl.duration` |

A record is self-contained: a subscriber needs nothing but the event to see
what failed, on which flow, how often and for how long.

## NMOS (IS-12: BCP-008-01 and BCP-008-02)

Worked out by [bcp008.h](../include/mxl-monitoring/bcp008.h)
([src/Bcp008.cpp](../src/Bcp008.cpp)) for the IS-12 implementation of an NMOS
Node, which holds the monitor objects and answers controllers. A reader or
replication source has a BCP-008-01 `NcReceiverMonitor`, a writer or
replication target a BCP-008-02 `NcSenderMonitor`. Nothing here is pushed or
served: the Node calls these functions, usually once per poll, and publishes
the result as monitor properties and their PropertyChanged notifications.

### Domain statuses (`mxlMonitoringBcp008GetStatuses`)

Judged on what was counted between two snapshots of one flow, the Node's
previous poll and the current one, so a failure recovered from within the
interval still shows. Each domain carries a status and a status message,
empty while healthy; a domain nothing in the interval decides is
`MXL_MONITORING_BCP008_UNCHANGED` (-1), and the Node keeps reporting what it
reported.

| Domain | `NcReceiverMonitor` | `NcSenderMonitor` | Values | Snapshot source |
| --- | --- | --- | --- | --- |
| link | `linkStatus` | `linkStatus` | AllUp (1), AllDown (3) | `access` (`domain_unavailable`) |
| connection | `connectionStatus` | `transmissionStatus` | Inactive (0), Healthy (1), Unhealthy (3) | `access`, `delivering`; since previous: `accessLosses`, `stalls`, failed reads or commits, `skipped`, `missing`, `too_early` |
| synchronization | `externalSynchronizationStatus` | `externalSynchronizationStatus` | NotUsed (0), always | none: media timestamps come from the node's own clock |
| stream | `streamStatus` | `essenceStatus` | Inactive (0), Healthy (1), PartiallyHealthy (2), Unhealthy (3) | since previous: receiver: essence issues and reads; sender: `invalid_input`, `input_late` and `ok` writes |

A disconnected flow is Inactive in connection and stream. The rules per
domain, and why, are in [DESIGN.md](../DESIGN.md#nmos).

### Counters (`mxlMonitoringBcp008GetCounters`)

Counted from a baseline snapshot, the one the Node keeps at the monitor's
last reset, to the current one.

| Monitor method | Counter names | Snapshot source | Unit |
| --- | --- | --- | --- |
| BCP-008-01 `GetLostPacketCounters` | `skipped`, `missing` | `skipped`, `essence` (`missing`) | grains or samples |
| BCP-008-01 `GetLatePacketCounters` | `too_early` | `reads` (`too_early`) | reads |
| BCP-008-02 `GetTransmissionErrorCounters` | `permission_denied`, `out_of_space`, `write_error` | `writes` by outcome | failed commits; failed attempts to write for `out_of_space` |

A counter kind the flow's side does not have is empty, which BCP-008 answers
with an empty collection.

### Left to the Node

The monitor object's own behaviour stays with the IS-12 implementation:
delaying reports by `statusReportingDelay`, `overallStatus` as the worst of
the domains, the status transition counters, Inactive on deactivation, and
`synchronizationSourceId`, which is `internal` since no external
synchronization is used. Across a restart of the process counting,
`mxlMonitoringBcp008Restarted` tells the Node to drop a baseline from before
it.
