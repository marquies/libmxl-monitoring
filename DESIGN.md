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

| Layer | Covers | Source |
| --- | --- | --- |
| Classification | every `mxlStatus` of an open, read or write -> access, read or write outcome | `mxlMonitoringClassify*` |
| Essence checks | format, resolution, rate, size, missing, invalid, incomplete, illegal values | the flow definition, `mxlGrainInfo`, the payload (`essence.h`) |
| Wrappers | libmxl reader and writer calls that classify, time and check their own result and return the status unchanged | `reader.h`, `writer.h` |
| Application reports | what no libmxl status carries: input late, out of space, invalid input | `mxlMonitoringRecordWriteOutcome`, `mxlMonitoringRecordWriteEssence` |
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

| Outcome | Values | IN-004 category |
| --- | --- | --- |
| Access | starting, open, domain unavailable, flow not found, permission denied, flow invalid, other, disconnected | Access Status |
| Read | ok, too early, too late, permission denied, error | Read Status |
| | invalid essence | Essence Status |
| Write | ok, permission denied, out of space, error | Write Status |
| | input late, invalid input | Essence Status |
| Essence | wrong format, wrong resolution, wrong rate, wrong size, missing, invalid, incomplete, illegal values | Essence Status, the reason for invalid essence or invalid input |

IN-004 sorts a reader's failures into Access, Read and Essence Status and a
writer's into Access, Write and Essence Status, and puts "nothing available in
time to write", incorrect input and grains "available but wrong" under Essence
Status. The model reports input late, invalid input and invalid essence as
read or write outcomes all the same: each takes the place of an ok read or
write, so a sum over the outcomes is the number of reads or writes, and every
exporter keeps that shape.

OK reads and writes, invalid essence, invalid input, input late and out of
space count units: grains for discrete flows, samples for continuous ones.
Every other outcome counts occurrences, since a failed read covers no data.
Essence issues count where read and write outcomes count, so a grain is either
ok or invalid essence, never both, and the essence counts say why.

Per flow the model also holds skipped units, stalls, opens, losses of
access, the last error (status, index, time, text), how the flow's definition
differs from its expectation, the age of the last data, whether it is
delivering, and three latency histograms.

## Counting rules

Each rule exists because the plain count misleads:

| Rule | Reason |
| --- | --- |
| FLOW_NOT_FOUND, FLOW_INVALID, INVALID_FLOW_READER and INVALID_FLOW_WRITER from a read or write count as a loss of access, once per open | the flow went away under an open reader or writer; counting each retry as a failed read would turn one restart into thousands of errors |
| FLOW_INVALID when opening counts as `other`, not as a loss | replaced data is a loss only for a reader that already had the flow open |
| An open that fails counts as domain unavailable when the domain directory is missing, is not a directory, or cannot be read or traversed | an instance keeps the domain it was created on, and libmxl reports a flow in a domain that went away since as not found; the domain is the cause, not the flow |
| An open that fails with a status other than not found or permission denied counts as permission denied when the flow's directory in the domain exists but cannot be read or traversed | a permission problem is reported as one, whatever status libmxl returned for it |
| TIMEOUT is classified as TOO_EARLY | a blocking read that timed out found nothing to read yet |
| TOO_EARLY counts once per index, and only once the index is a grace period past its media timestamp | a reader at the head waits for the next grain on every read; that is normal, not a fault |
| TOO_EARLY does not count while the rate is unknown | without a rate there is no media timestamp to measure the grace against |
| A stall counts once per stretch without data; reopening or retargeting the flow does not start a new one | a flow that stays dead across reopens is one outage |
| A read failing for a reason other than timing sets access to permission denied or other until data moves or the flow is reopened | such a reader is unusable, and the access state should say so |
| Out of space is taken from `errno == ENOSPC` or a filesystem with no space left | no libmxl status means a full domain; the writer wrapper checks both after a failed create |

The reader wrappers derive grace and stall timeout on every open from the
flow, unless `mxlMonitoringSetFlowTiming` set them: data may land one read
span after its timestamp, or for samples the writer's commit batch if larger,
plus 50 ms for scheduling. A read spans one grain, or 10 ms of samples or the
writer's sync batch if larger. A stall is four read spans without data, at
least 100 ms, so a grain committed late is a slow grain rather than a stall.

## Essence checks

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
doubling on to 81.92 s, then one bucket above. The long tail is there for
timestamps far from the reader's clock, such as a source stamping from a clock
of its own: a quantile that falls above the last bound reads as that bound. Nothing is observed while the rate is unknown. Negative
latencies, from clocks apart between nodes, count as 0. Arrival and commit on
a reader use the reader node's clock, so their difference is free of clock
offset between nodes.

## Conditions and events

A condition is one way a flow is failing: one access failure, one read or
write outcome, or, for invalid essence and invalid input, one essence issue,
so a wrong format and illegal values notify separately. Each condition emits:

| Phase | When |
| --- | --- |
| started | its first occurrence |
| persisting | once per interval (60 s by default) while it recurs, or while the flow delivers nothing and is not disconnected |
| recovered | access: the flow is open again, or disconnected on purpose; read and write: no recurrence for the recover delay (1 s by default), good data moved since the last occurrence, and the flow delivers |

One event per change instead of one per grain: at 50 grains a second a
per-grain event floods whatever receives it. An event carries the last libmxl
status, the last affected index, occurrences since the previous event, the
total and duration since started, a text, and the flow's format and unit, so
a subscriber needs nothing but the event. Recovery requires good data and
delivering so that a condition does not recover merely because its failing
calls stopped, for example while the process waits to reopen.

Events are produced in `mxlMonitoringPoll` and delivered to subscribers on the
polling thread. A notification is late by at most the poll interval, which has
to be shorter than the recover delay.

## Exporters

### Prometheus

`mxl-monitoring-prometheus` renders the text exposition format from a
snapshot. The reader's names predate the library: reader flows reproduce what
`mxl-reader` serves, so dashboards and alert rules built on it keep working.
The one exception is `access_losses_total`, which `mxl-reader` calls
`lost_total`: BCP-008-01 calls lost what never arrived, skipped and missing
grains, and a loss of access is not that. Other roles follow the same scheme
under their own prefix, with the label values of the OTLP exporter:

| Role | Prefix |
| --- | --- |
| reader | `mxl_reader` |
| writer | `mxl_writer` |
| replication source | `mxl_replication_source` |
| replication target | `mxl_replication_target` |

| Metric | Type | Roles |
| --- | --- | --- |
| `<prefix>_flow_state{flow,state}` | gauge | all |
| `<prefix>_flow_delivering{flow}` | gauge | all |
| `<prefix>_reads_total{flow,result}` | counter | reader, source |
| `<prefix>_writes_total{flow,result}` | counter | writer, target |
| `<prefix>_invalid_essence_total{flow,issue}` | counter | source |
| `<prefix>_invalid_input_total{flow,issue}` | counter | writer, target |
| `<prefix>_open_failures_total{flow,reason}` | counter | all |
| `<prefix>_skipped_total{flow}`, `<prefix>_stalls_total{flow}` | counter | reader, source |
| `<prefix>_opens_total{flow}`, `<prefix>_access_losses_total{flow}` | counter | all |
| `<prefix>_age_seconds{flow}` | gauge | all |
| `<prefix>_arrival_seconds` | histogram | reader, source |
| `<prefix>_commit_seconds` | histogram | all |
| `<prefix>_input_seconds` | histogram | writer, target |

The reader's `result` label keeps `mxl-reader`'s values: essence issues
appear as their own results, and errors and open failure reasons under names
of their own. The OTLP names are canonical; the reader's values map to them
as follows, and every other role uses the canonical values, with a source's
essence issues in `invalid_essence_total{issue}` as a writer's are in
`invalid_input_total{issue}`:

| Label | `mxl_reader` value | Canonical value |
| --- | --- | --- |
| `result` | `ok`, `too_early`, `too_late` | the same |
| `result` | `permission_error` | `permission_denied` |
| `result` | `other_error` | `read_error` |
| `result` | `missing`, `invalid`, `incomplete`, `wrong_size`, `illegal_values` | `invalid_essence` with that issue |
| `result` | `wrong_format` | `invalid_essence` with issue `wrong_format`, `wrong_resolution` or `wrong_rate`, summed |
| `reason` | `domain` | `domain_unavailable` |
| `reason` | `not_found` | `flow_not_found` |
| `reason` | `permission` | `permission_denied` |
| `reason` | `other` | `flow_invalid` and `other`, summed |

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

### OpenTelemetry

`mxl-monitoring-otlp` pushes over OTLP/HTTP with opentelemetry-cpp, configured
only through the standard `OTEL_*` environment variables. gRPC is not built:
it would add gRPC to every image linking the exporter, and a Collector takes
both.

Metrics are built from snapshots on the exporter's own thread every
`OTEL_METRIC_EXPORT_INTERVAL` and handed to the SDK's OTLP exporter as
finished data points, cumulative from the exporter's start. The SDK's
instruments do not fit: it has no asynchronous histogram, and the snapshot
already holds every histogram complete. A Collector that cannot be reached
costs the points of that interval, never a count.

| Metric | Type | Attributes besides the flow's | Roles |
| --- | --- | --- | --- |
| `mxl.flow.state` | gauge, `{state}` | `mxl.flow.state`: the access names | reader, writer |
| `mxl.flow.open.failures` | counter | `mxl.open.failure`: `domain_unavailable`, `flow_not_found`, `permission_denied`, `flow_invalid`, `other` | reader, writer |
| `mxl.flow.opens` | counter, `{open}` | | reader, writer |
| `mxl.flow.access.losses` | counter, `{loss}` | | reader, writer |
| `mxl.reader.reads` | counter | `mxl.read.result`: `ok`, `too_early`, `too_late`, `permission_denied`, `read_error`, `invalid_essence` with `mxl.essence.issue` | reader |
| `mxl.reader.skipped`, `mxl.reader.stalls` | counter | | reader |
| `mxl.reader.delivering` | gauge, `{flow}` | | reader |
| `mxl.reader.age` | gauge, `s` | | reader |
| `mxl.reader.arrival.latency`, `mxl.reader.commit.latency` | histogram, `s` | | reader |
| `mxl.writer.writes` | counter | `mxl.write.result`: `ok`, `input_late`, `permission_denied`, `out_of_space`, `write_error`, `invalid_input` with `mxl.essence.issue` | writer |
| `mxl.writer.delivering` | gauge, `{flow}` | | writer |
| `mxl.writer.head` | gauge, `{index}` | | writer |
| `mxl.writer.commit.latency`, `mxl.writer.input.latency` | histogram, `s` | | writer |

Every point carries `mxl.flow.id`, and once the flow is open
`mxl.flow.media_type` (`video`, `audio`, `data`, `event`) and `mxl.flow.unit`
(`grain`, `sample`). Essence issues are counted per issue and never as a total
beside them, so a sum over the result is the number of reads or writes.
Counters of grains, samples or reads have unit `1`: an instrument has one
unit, and a flow's unit is its attribute. No gauge has unit `1`, which Prometheus would name with a `_ratio`
suffix. `mxl.status` is not on metric points, since a snapshot keeps only the
last status of a flow, not one per outcome; events carry it. Replication roles
are not exported yet.

Each event is one log record with an event name, `mxl.flow.open_failed`,
`mxl.flow.open_failing` and `mxl.flow.opened` for access, `mxl.reader.read_failed`
and `mxl.reader.read_recovered`, `mxl.writer.write_failed` and
`mxl.writer.write_recovered`; severity WARN for `too_early`, `too_late` and
`input_late`, ERROR for other failures, INFO for a recovery; a one-sentence
body; and the flow's attributes with the outcome, `mxl.essence.issue`,
`mxl.event.phase`, `mxl.status`, `mxl.grain.index`, `mxl.occurrences`,
`mxl.total` and `mxl.duration`. Records leave through the SDK's batch
processor, so delivering an event never waits on the network.

### NMOS

`bcp008.h`, in the core, decides what an AMWA monitor reports for a flow: a
[BCP-008-01](https://specs.amwa.tv/bcp-008-01/latest/) `NcReceiverMonitor` for
a reader or replication source, a
[BCP-008-02](https://specs.amwa.tv/bcp-008-02/latest/) `NcSenderMonitor` for a
writer or replication target. It reads snapshots and
nothing else, so it carries no NMOS dependency, and a Node that polls another
process's counts fills snapshots from them and gets the same judgement.

The IS-12 implementation of the Node holds the monitor object and does what
BCP-008-01 and BCP-008-02 ask of the object rather than of the judgement: it
delays reports by `statusReportingDelay`, counts status transitions, derives
`overallStatus`, and turns a monitor `Inactive` on deactivation. The library
hands it domain statuses and counters.

The Node also sets `synchronizationSourceId`, which `bcp008.h` does not
provide: to `internal`. MXL media timestamps come from the node's own clock,
so the monitor uses no external synchronization, and both specifications
require `internal` of a device that does not intend to use it; the
synchronization status is NotUsed to match.

Statuses are judged on what was counted between two snapshots, the previous
poll's and the current one. A fault counted before the previous poll has
been reported once; judging on totals would hold a monitor unhealthy for the
rest of the process's life. Counts are read as well as states, because a
failure the flow recovered from within the interval, a failed read, a loss
of access and reopen or a stall shorter than a second, has left no state by the time
of the poll. A domain nothing in the interval decides, such as the stream of
a reader that read nothing, is reported unchanged: no data says nothing
about whether data decodes.

| Domain | Receiver | Sender |
| --- | --- | --- |
| link | AllDown while the MXL domain is unavailable, the flow's only link; AllUp otherwise | same |
| connection or transmission | Unhealthy while not open or not delivering, or for a loss of access, a stall, a failed read, grains lost (skipped, missing) or late reads (too early past the grace) | Unhealthy while not open or not delivering, or for a loss of access, a stall or a failed commit |
| synchronization | NotUsed, with `synchronizationSourceId` `internal`: media timestamps come from the node's own clock | same |
| stream or essence | Unhealthy for wrong format, wrong size or invalid; PartiallyHealthy for wrong resolution, wrong rate, incomplete or illegal values | Unhealthy for invalid input or no input in time at all, PartiallyHealthy for some input late |

A disconnected flow is Inactive in connection and stream. BCP-008-01 calls a
stream Unhealthy when it cannot be decoded and PartiallyHealthy when it
decodes but differs from what was expected; that line separates the essence
issues. A missing grain was never written, so it counts against the
connection as lost, not against the stream, and reading only missing grains
leaves the stream unchanged. On a sender, a grain written without input in
time carries no essence: with none in time at all the sender has no essence,
which BCP-008-02 calls Unhealthy; with some, the essence is there with gaps.

BCP-008-01 calls lost what never arrived and late what arrived too late to be
used. To a reader, a grain the ring overtook it on never arrived, whatever
the writer did, so skipped counts as lost, as does missing; a read that found
its grain not there a grace period past its media timestamp is late, and it
counts in reads, since the core counts too early once per read index. In this
library "lost" means only that; a flow going away under an open reader or
writer is a loss of access (`accessLosses`, `access_losses_total`,
`mxl.flow.access.losses`).

| BCP counter | Kind | Counters, by name | Unit |
| --- | --- | --- | --- |
| BCP-008-01 `GetLostPacketCounters` | `MXL_MONITORING_BCP008_LOST` | `skipped`, `missing` | grains or samples |
| BCP-008-01 `GetLatePacketCounters` | `MXL_MONITORING_BCP008_LATE` | `too_early`: too-early reads past the grace period | reads |
| BCP-008-02 `GetTransmissionErrorCounters` | `MXL_MONITORING_BCP008_TRANSMISSION_ERROR` | failed commits by outcome: `permission_denied`, `out_of_space`, `write_error` | failed commits; failed attempts to write for `out_of_space` |

A `too_late` read counts in neither a counter nor a status of its own. It is
not a failed read, so it does not make the connection Unhealthy and appears
in no status message; the grains the reader then jumps over to reach the head
again, recorded with `mxlMonitoringRecordSkipped`, count as lost, in the
connection status (`<n> grains lost`) and in the lost counters. A reader that
reads nothing but `too_late` stops delivering and turns the connection
Unhealthy through that.

Counters come from a baseline snapshot, the one a Node keeps at the monitor's
last reset, as in the table. Input late and invalid input belong to the
essence, not the transmission. A kind the flow's side does not have is empty,
which is how BCP-008-01 and BCP-008-02 answer a device without that
capability.

Counts never go down within a process. A Node that polls another process's
counts sees them go down when that process starts again, and then takes
every count of the current snapshot as new, in statuses and counters alike.
That holds only while some count is still below the baseline, so the Node
checks each poll with `mxlMonitoringBcp008Restarted` and drops a counter
baseline from before a restart.

## Health

Status: design, not implemented.

The NMOS judgement reaches only an IS-12 Node. Prometheus and OTLP carry the
counts, and every dashboard and alert rule decides on its own what makes a
flow unhealthy. The core judges each flow once, with the rules of the NMOS
section, and every exporter publishes that judgement.

### Judgement

A flow's health is the worst of its domain statuses, as BCP-008-01 and
BCP-008-02 derive `overallStatus`: Inactive while the flow is disconnected, Healthy only while
every domain is Healthy or NotUsed, and a link AllDown counts as Unhealthy.
The domain statuses are those of `mxlMonitoringBcp008GetStatuses`; the core
calls the same code, so a Node and an exporter never disagree on a rule.

The core judges at every poll, on the counts since the start of the current
window: one second, the span `delivering` already uses. At the end of a
window the current snapshot becomes the next window's start. A poll judges a
partial window, so a fault is seen at the poll after it, not a second later.
A domain the window does not decide, reported unchanged, keeps the status
the core last judged for it.

### Limits

The NMOS rules tolerate nothing: one lost grain or one late read makes the
connection Unhealthy. A reader that the ring overtakes once an hour then
flaps. Limits make some of that tolerable:

| Limit | Applies to | Unit |
| --- | --- | --- |
| `lost` | receiver connection: skipped and missing | share of the grains or samples read in the window |
| `late` | receiver connection: too early past the grace | share of the reads in the window |
| `inputLate` | sender essence: input late | share of the writes in the window |

A count at or below its limit judges as if it were zero; above it, as the
rules say. Losses of access, stalls, failed reads and commits, out of space
and every essence issue keep no limit: each is a fault, however rare. A share rather
than a count per window keeps a limit independent of the rate and the window.

Every limit is 0 by default, which is the NMOS judgement as it is. The limits
are a field of `mxlMonitoringConfig` for the core, and an argument of a
variant of `mxlMonitoringBcp008GetStatuses` for a Node, so a Node judging
with the same limits reports what the exporters report. The library reads no
file; a media function fills them from its own configuration.

### Writes without input

A sender that fills a gap in its input, with black, silence or a repeated
grain, still commits a grain. Today only an `ok` write counts as data that
moved, so a sender recording a fill as `input_late` alone stops delivering
and its transmission turns Unhealthy; to stay delivering it records the fill
as `ok` and as `input_late`, and a sender with no input at all then reads
PartiallyHealthy, as if some input had arrived.

An `input_late` write counts as data that moved, as `ok` does, and a sender
records a fill as `input_late` only. A sender without input then reads
transmission Healthy, since it commits every grain, and essence Unhealthy,
since none of them carries input; one with gaps reads PartiallyHealthy. The
counts stay as they are, so `ok` keeps meaning a grain written from input.

### Reporting delay

BCP-008-01 and BCP-008-02 report a change to a less healthy status at once, and a change to a
healthier one only once it has held for `statusReportingDelay`, 3 s by
default. The core applies the same rule to the health it exports, with
`statusReportingDelayNs` in the configuration, 0 meaning 3 s, so a dashboard sees what
a controller sees. `mxlMonitoringBcp008GetStatuses` stays undelayed: the
Node holds the monitor object and applies the delay itself, and a second
delay would double it.

### Export

| Exporter | Form |
| --- | --- |
| snapshot | `health`, the delayed status, and `healthChangedNs`; snapshot version 3, which the NMOS calls accept beside version 2 |
| Prometheus | `<prefix>_flow_health{flow,status}`, one series per status with 1 for the current one, as `flow_state` |
| OTLP | `mxl.flow.health`, gauge, attribute `mxl.flow.health` with the status name |
| events | none: a flow turns less healthy through a condition, which emits its own event |

Status names: `inactive`, `healthy`, `partially_healthy`, `unhealthy`.

## What libmxl does not provide

| Gap | Effect here |
| --- | --- |
| No status for a full domain | out of space depends on the caller's `errno` |
| No observer hook for reads, writes and opens | coverage needs the wrappers or explicit record calls |
| One `lastReadTime` per flow, not per reader | each reader reports its own reads |
| No per-grain commit time | commit latency on a reader is measured only while the grain read is the head |
| No latency or jitter measurement, which IN-004 asks of media functions | latency is measured here against media timestamps; jitter is not measured, and IN-004 has no common method of calculating it yet |
