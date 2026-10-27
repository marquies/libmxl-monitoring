# Design

Why libmxl-monitoring is built the way it is: what it has to detect, the model
it records into, the rules that turn libmxl statuses into counts and events,
and how exporters present them. `README.md` covers use and build; the headers
in `include/mxl-monitoring/` are the reference for every call.

## What has to be detected

For every process that reads, writes or replicates an MXL flow:

| Who | Failure |
| --- | --- |
| Reader and writer | cannot access the domain; cannot access the flow |
| Reader | data not available yet (too early); data overwritten before it was read (too late); available but cannot be read, because of permissions or otherwise; available but wrong |
| Writer | nothing available in time to write; incorrect input; correct input that cannot be written, because of permissions, a full domain, or otherwise |
| Replication | the same, for the reading side on the source node and the writing side on the target node |

Each failure is counted, so a backend can alert on rates, and notified as an
event, so a person sees what happened, to which flow, with which libmxl status.

## Placement

A separate library with wrappers around the libmxl calls, not
instrumentation inside libmxl. It needs no change to libmxl, and a process opts
in call by call. The API is C, in the style of libmxl, so it can be proposed
upstream and bound from Go through cgo; the implementation is C++20.

Every target is a static library. Processes sharing a domain must load the same
`libmxl.so`; linking the monitoring statically keeps libmxl the only shared
library they have to agree on, and the library never ships a libmxl of its
own. An exporter with heavy dependencies is its own target, so a process
carries those dependencies only when it links that exporter.

## Layers

| Layer | Content | Source |
| --- | --- | --- |
| Classification | every `mxlStatus` of an open, read or write -> access, read or write outcome | `mxlMonitoringClassify*` |
| Content checks | format, resolution, rate, size, missing, invalid, incomplete, illegal values | the flow definition, `mxlGrainInfo`, the payload (`content.h`) |
| Wrappers | libmxl reader calls that classify, time and check their own result and return the status unchanged | `reader.h` |
| Application reports | what no libmxl status carries: input late, out of space, invalid input | `mxlMonitoringRecordWriteOutcome`, `mxlMonitoringRecordWriteContent` |
| Core model | per-flow counters and histograms, access state, last error, conditions | `monitoring.h` |
| Snapshots and events | a copy of every flow's counts; started, persisting and recovered events | `mxlMonitoringSnapshot`, `mxlMonitoringPoll` |
| Exporters | render snapshots and events for a backend | `prometheus.h` |

## Hot path

A record call touches relaxed atomics of its own flow and nothing else: no
lock, no allocation, no event state. One thread records for a given flow.
Everything that needs history happens in `mxlMonitoringPoll`, on one thread,
by comparing each flow's counters with those of the previous poll. A read or
write loop therefore never waits for an exporter, and an exporter reads
snapshots on its own schedule.

## Flows and targets

A flow handle exists before the flow is opened, so a failure to reach the flow
has somewhere to be counted. The handle is reported under the id it was added
with. By default that id is also the libmxl flow it opens;
`mxlMonitoringSetFlowTarget` points it at another. This serves a reader slot
that an NMOS controller connects and disconnects: the slot keeps its name and
its counts while the flow behind it changes, and its access is `disconnected`
while it reads nothing. A change of target forgets what was known of the
previous flow (format, definition, last data, last error) and keeps the counts.

## Model

| Outcome | Values |
| --- | --- |
| Access | starting, open, domain unavailable, flow not found, permission denied, flow invalid, other, disconnected |
| Read | ok, too early, too late, permission denied, error, invalid content |
| Write | ok, input late, invalid input, permission denied, out of space, error |
| Content | wrong format, wrong resolution, wrong rate, wrong size, missing, invalid, incomplete, illegal values |

OK reads and writes, invalid content, invalid input, input late and out of
space count units: grains for discrete flows, samples for continuous ones.
Every other outcome counts occurrences, since a failed read covers no data.
Content issues count where read and write outcomes count, so a grain is either
ok or invalid content, never both, and the content counts say why.

Per flow the model also holds skipped units, stalls, opens, losses, the last
error (status, index, time, text), how the flow's definition differs from its
expectation, the age of the last data, whether it is delivering, and three
latency histograms.

## Counting rules

Each rule exists because the plain count misleads:

| Rule | Reason |
| --- | --- |
| FLOW_NOT_FOUND, FLOW_INVALID, INVALID_FLOW_READER and INVALID_FLOW_WRITER from a read or write count as a loss of access, once per open | the flow went away under an open reader or writer; counting each retry as a failed read would turn one restart into thousands of errors |
| FLOW_INVALID when opening counts as `other`, not as a loss | replaced data is a loss only for a reader that already had the flow open |
| An open that fails with a status other than not found or permission denied counts as permission denied when the flow's directory in the domain exists but cannot be read or traversed | a permission problem is reported as one, whatever status libmxl returned for it |
| TIMEOUT is classified as TOO_EARLY | a blocking read that timed out found nothing to read yet |
| TOO_EARLY counts once per index, and only once the index is a grace period past its media timestamp | a reader at the head waits for the next grain on every read; that is normal, not a fault |
| TOO_EARLY does not count while the rate is unknown | without a rate there is no media timestamp to measure the grace against |
| A stall counts once per stretch without data; reopening or retargeting the flow does not start a new one | a flow that stays dead across reopens is one outage |
| A read failing for a reason other than timing sets access to permission denied or other until data moves or the flow is reopened | such a reader is unusable, and the access state should say so |
| Out of space is reported by the caller | no libmxl status means a full domain; a writer has it from `errno == ENOSPC` |

The reader wrappers derive grace and stall timeout on every open from the
flow, unless `mxlMonitoringSetFlowTiming` set them: data may land one read
span after its timestamp, or for samples the writer's commit batch if larger,
plus 50 ms for scheduling. A read spans one grain, or 10 ms of samples or the
writer's sync batch if larger. A stall is four read spans without data, at
least 100 ms, so a grain committed late is a slow grain rather than a stall.

## Content checks

A grain or batch counts at most one issue, the worst, so a grain that is both
the wrong size and incomplete counts once. Format, resolution and rate are
compared once per open between the flow definition and the expectation; the
result is applied to every grain read while it differs.

`missing` and `invalid` are kept apart. `missing` means the ring slot holds
another index's grain, so this one was never written; `invalid` means the grain
carries `MXL_GRAIN_FLAG_INVALID`. libmxl sets that flag itself on the grains a
writer jumps over, and clears it when a grain is opened for writing, so a gap
behind a writer's earlier commit reads as `invalid`, and `missing` is left for a
slot no writer has reached. With `MXL_GRAIN_VALID_SLICES_ALL`, libmxl answers
TOO_EARLY for a slot that was never written; only a read with
`MXL_GRAIN_VALID_SLICES_ANY` returns the stale grain that the check reports as
`missing`.

The legal-value check is off unless the expectation asks for it: a test
pattern can carry reserved codes on purpose, and a check that always fires
hides the faults it exists to find.

## Latency

| Histogram | Measured |
| --- | --- |
| arrival | reader: read time minus the media timestamp of what was read |
| commit | reader: the flow's last write time minus the media timestamp, only while the grain read is the head, the one moment libmxl says when a particular grain was written; writer: commit time minus media timestamp |
| input | writer: input arrival minus the media timestamp it is written at |

Bucket bounds are 1, 2, 5, 10, 20, 40, 80, 160, 320, 640 and 1280 ms, then
one bucket above. Nothing is observed while the rate is unknown. Negative
latencies, from clocks apart between nodes, count as 0. Arrival and commit on
a reader use the reader node's clock, so their difference is free of clock
offset between nodes.

## Conditions and events

A condition is one way a flow is failing: one access failure, one read or
write outcome, or, for invalid content and invalid input, one content issue,
so a wrong format and illegal values notify separately. Each condition emits:

| Phase | When |
| --- | --- |
| started | its first occurrence |
| persisting | once per interval (60 s by default) while it recurs, or while the flow delivers nothing and is not disconnected |
| recovered | access: the flow is open again, or disconnected on purpose; read and write: no recurrence for the recover delay (1 s by default), good data moved since the last occurrence, and the flow delivers |

One event per change instead of one per grain: at 50 grains a second a
per-grain event floods whatever receives it. An event carries the last libmxl
status, the last affected index, occurrences since the previous event, the
total and duration since started, and a text. Recovery requires good data and
delivering so that a condition does not recover merely because its failing
calls stopped, for example while the process waits to reopen.

Events are produced in `mxlMonitoringPoll` and delivered to subscribers on the
polling thread. A notification is late by at most the poll interval, which has
to be shorter than the recover delay.

## Exporters

### Prometheus

`mxl-monitoring-prometheus` renders the text exposition format from a
snapshot. The names predate the library: reader flows reproduce exactly what
`mxl-reader` serves, so dashboards and alert rules built on it keep working.
New roles follow the same scheme under their own prefix:

| Role | Prefix |
| --- | --- |
| reader | `mxl_reader` |
| writer | `mxl_writer` |
| replication source | `mxl_gateway_source` |
| replication target | `mxl_gateway_target` |

| Metric | Type | Roles |
| --- | --- | --- |
| `<prefix>_flow_state{flow,state}` | gauge | all |
| `<prefix>_flow_delivering{flow}` | gauge | all |
| `<prefix>_reads_total{flow,result}` | counter | reader, source |
| `<prefix>_writes_total{flow,result}` | counter | writer, target |
| `<prefix>_invalid_input_total{flow,issue}` | counter | writer, target |
| `<prefix>_open_failures_total{flow,reason}` | counter | all |
| `<prefix>_skipped_total{flow}`, `<prefix>_stalls_total{flow}` | counter | reader, source |
| `<prefix>_opens_total{flow}`, `<prefix>_lost_total{flow}` | counter | all |
| `<prefix>_age_seconds{flow}` | gauge | all |
| `<prefix>_arrival_seconds` | histogram | reader, source |
| `<prefix>_commit_seconds` | histogram | all |
| `<prefix>_input_seconds` | histogram | writer, target |

The read `result` label keeps `mxl-reader`'s values: content issues appear as
their own results (`missing`, `invalid`, `incomplete`, `wrong_size`,
`illegal_values`, and `wrong_format` covering format, resolution and rate),
next to `ok`, `too_early`, `too_late`, `permission_error` and `other_error`.
Open failure reasons are `domain`, `not_found`, `permission` and `other`.
Flow labels are escaped as the exposition format requires, never sanitised: a
slot named `cam:video` stays `cam:video`.


### HTTP endpoint

`mxl-monitoring-http` serves the Prometheus text for a process that has no
HTTP server of its own: GET `/metrics` and GET `/healthz`, nothing else. One
thread answers one request at a time, without keep-alive, and reading a
request and sending its answer each get 200 ms, so a client that sends nothing
or reads slowly holds the next one up by that much at most. Stopping shuts the
listening socket down, which wakes the thread at once. A process with a server
of its own serves `mxlMonitoringPrometheusRender` from it instead; OTLP pushes
to a collector and needs no server in the process, and an NMOS node brings its
own.

### OpenTelemetry and NMOS

Not implemented. The model is shaped so that both fit as further exporters
over the same snapshots and events:

- OTLP: counters, gauges and histograms read from snapshots as observable
  instruments, events as log records, configured through the standard
  `OTEL_*` environment variables, built on opentelemetry-cpp as its own
  target.
- NMOS BCP-008: link, connection and stream status of the receiver and sender
  monitors derived from access, delivering and the read or write outcomes.
  BCP-008 has the device judge its own health, so that judgement belongs in
  the core, shared by every exporter, rather than in this exporter alone.

## What libmxl does not provide

| Gap | Effect here |
| --- | --- |
| No status for a full domain | out of space depends on the caller's `errno` |
| No observer hook for reads, writes and opens | coverage needs the wrappers or explicit record calls |
| One `lastReadTime` per flow, not per reader | each reader reports its own reads |
| No per-grain commit time | commit latency on a reader is measured only while the grain read is the head |
