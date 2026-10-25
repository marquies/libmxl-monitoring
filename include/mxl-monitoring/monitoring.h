// The core model: one context per process, one flow handle per flow the
// process reads or writes. Readers and writers record what happened to a flow;
// exporters take snapshots and subscribe to events.
//
// Every mxlMonitoringRecord* call touches atomics of its own flow only, never
// blocks and never allocates, so it may run inside a read or write loop. One
// thread is expected to record for a given flow. Turning counts into events
// happens in mxlMonitoringPoll, on one thread, by comparing the counts with
// those of the previous poll, so the hot path carries no event state.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mxl/dataformat.h>
#include <mxl/mxl.h>
#include <mxl/rational.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct mxlMonitoring_t* mxlMonitoring;
    typedef struct mxlMonitoringFlow_t* mxlMonitoringFlow;

    // The replication gateway reads a flow on the source node and writes it on
    // the target node: a source grain overwritten before it was sent is a
    // TOO_LATE read, a failed commit on the target a failed write. The role
    // tells those series apart from a media function's.
    typedef enum mxlMonitoringRole
    {
        MXL_MONITORING_ROLE_READER,
        MXL_MONITORING_ROLE_WRITER,
        MXL_MONITORING_ROLE_REPLICATION_SOURCE,
        MXL_MONITORING_ROLE_REPLICATION_TARGET,
    } mxlMonitoringRole;

    typedef enum mxlMonitoringUnit
    {
        MXL_MONITORING_UNIT_UNKNOWN, // the flow has not been opened yet
        MXL_MONITORING_UNIT_GRAIN,   // discrete flows
        MXL_MONITORING_UNIT_SAMPLE,  // continuous flows
    } mxlMonitoringUnit;

    // Whether the flow can be reached, and if not, why.
    typedef enum mxlMonitoringAccess
    {
        MXL_MONITORING_ACCESS_STARTING,
        MXL_MONITORING_ACCESS_OPEN,
        MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE,
        MXL_MONITORING_ACCESS_FLOW_NOT_FOUND,
        MXL_MONITORING_ACCESS_PERMISSION_DENIED,
        // The flow's data was replaced, typically by a writer that restarted.
        MXL_MONITORING_ACCESS_FLOW_INVALID,
        MXL_MONITORING_ACCESS_OTHER,
        // No flow to read or write: the handle's target was cleared on
        // purpose, e.g. by an NMOS controller disconnecting a receiver.
        MXL_MONITORING_ACCESS_DISCONNECTED,
        MXL_MONITORING_ACCESS_COUNT,
    } mxlMonitoringAccess;

    // One word per access state, as mxl-reader has named them: starting,
    // open, domain_unavailable, flow_not_found (also for a flow whose data
    // was replaced), permission_denied, flow_error, disconnected.
    char const* mxlMonitoringAccessName(mxlMonitoringAccess access);

    typedef enum mxlMonitoringRead
    {
        MXL_MONITORING_READ_OK,
        MXL_MONITORING_READ_TOO_EARLY,
        MXL_MONITORING_READ_TOO_LATE,
        MXL_MONITORING_READ_PERMISSION_DENIED,
        MXL_MONITORING_READ_ERROR,
        // Read, but wrong; the mxlMonitoringContent counts say how.
        MXL_MONITORING_READ_INVALID_CONTENT,
        MXL_MONITORING_READ_COUNT,
    } mxlMonitoringRead;

    typedef enum mxlMonitoringWrite
    {
        MXL_MONITORING_WRITE_OK,
        // Nothing arrived in time to write.
        MXL_MONITORING_WRITE_INPUT_LATE,
        // The input was wrong; the mxlMonitoringContent counts say how.
        MXL_MONITORING_WRITE_INVALID_INPUT,
        MXL_MONITORING_WRITE_PERMISSION_DENIED,
        MXL_MONITORING_WRITE_OUT_OF_SPACE,
        MXL_MONITORING_WRITE_ERROR,
        MXL_MONITORING_WRITE_COUNT,
    } mxlMonitoringWrite;

    // How content is wrong, for what a reader read and what a writer was given.
    typedef enum mxlMonitoringContent
    {
        MXL_MONITORING_CONTENT_WRONG_FORMAT,     // media type, channels or another format property
        MXL_MONITORING_CONTENT_WRONG_RESOLUTION, // width or height
        MXL_MONITORING_CONTENT_WRONG_RATE,       // grain or sample rate
        MXL_MONITORING_CONTENT_WRONG_SIZE,       // payload size does not match the format
        // The ring slot holds another index's grain: this one was never written.
        MXL_MONITORING_CONTENT_MISSING,
        // Marked MXL_GRAIN_FLAG_INVALID by its writer, e.g. to fill a gap.
        MXL_MONITORING_CONTENT_INVALID,
        MXL_MONITORING_CONTENT_INCOMPLETE,     // fewer valid slices than total
        MXL_MONITORING_CONTENT_ILLEGAL_VALUES, // values outside the legal range of the format
        MXL_MONITORING_CONTENT_COUNT,
    } mxlMonitoringContent;

    // What a libmxl status means for each kind of call.
    //
    // MXL_ERR_FLOW_NOT_FOUND, MXL_ERR_FLOW_INVALID, MXL_ERR_INVALID_FLOW_READER
    // and MXL_ERR_INVALID_FLOW_WRITER returned by a read or write mean the flow
    // went away under an open reader or writer; mxlMonitoringRecordRead and
    // mxlMonitoringRecordWrite count them as a loss of access rather than as a
    // read or write outcome. TIMEOUT is classified as TOO_EARLY: a
    // blocking read that timed out found nothing to read yet. No status means
    // out of space; that is the caller's to report.
    mxlMonitoringAccess mxlMonitoringClassifyAccess(mxlStatus status);
    mxlMonitoringRead mxlMonitoringClassifyRead(mxlStatus status);
    mxlMonitoringWrite mxlMonitoringClassifyWrite(mxlStatus status);

    typedef struct mxlMonitoringConfig_t
    {
        uint32_t version; // 1
        uint32_t size;    // sizeof(mxlMonitoringConfig)

        // A condition that keeps occurring emits one PERSISTING event per
        // interval. 0 means 60 s.
        uint64_t persistIntervalNs;
        // A read or write condition is over once it has not recurred for this
        // long, good data moved since it last did, and the flow delivers. 0
        // means 1 s.
        uint64_t recoverDelayNs;

        // TAI nanoseconds, the clock of media timestamps. NULL uses
        // mxlGetTime. Called from every recording thread and the polling
        // thread at once.
        uint64_t (*now)(void* user);
        void* nowUser;
    } mxlMonitoringConfig;

    // config may be NULL for the defaults.
    mxlStatus mxlMonitoringCreate(mxlMonitoringConfig const* config, mxlMonitoring* monitoring);
    // Frees every flow with it; their handles are invalid afterwards.
    mxlStatus mxlMonitoringDestroy(mxlMonitoring monitoring);

#define MXL_MONITORING_ID 256

    // A flow is known by its id before it is opened, so a failure to reach it
    // has a flow to belong to. The id is what the flow is reported as, up to
    // MXL_MONITORING_ID - 1 characters; it is also the libmxl flow it opens
    // unless mxlMonitoringSetFlowTarget names another.
    mxlStatus mxlMonitoringAddFlow(mxlMonitoring monitoring, char const* flowId, mxlMonitoringRole role, mxlMonitoringFlow* flow);
    // No record call may run on the flow during or after its removal. Its
    // active conditions end without an event.
    mxlStatus mxlMonitoringRemoveFlow(mxlMonitoring monitoring, mxlMonitoringFlow flow);

    // Points the handle at another libmxl flow, for a reader slot that is
    // reported under its own name whichever flow it is connected to. NULL or
    // "" disconnects it. A change keeps every count and forgets what was
    // known of the flow before: its format, definition, last data, age and
    // last error. Call from the thread recording for the flow, between reads.
    mxlStatus mxlMonitoringSetFlowTarget(mxlMonitoringFlow flow, char const* flowId);

    // Where the flow stands now, without taking a snapshot.
    mxlMonitoringAccess mxlMonitoringGetFlowAccess(mxlMonitoringFlow flow);

    typedef struct mxlMonitoringFlowFormat_t
    {
        uint32_t version; // 1
        uint32_t size;    // sizeof(mxlMonitoringFlowFormat)

        mxlDataFormat format;
        mxlMonitoringUnit unit;
        // Grain rate, or sample rate: turns an index into its media timestamp.
        mxlRational rate;
        // The flow definition's media_type, e.g. "video/v210". May be NULL.
        char const* mediaType;
    } mxlMonitoringFlowFormat;

    // What the flow is, once known. The reader wrappers set it on open.
    mxlStatus mxlMonitoringSetFlowFormat(mxlMonitoringFlow flow, mxlMonitoringFlowFormat const* format);

    // When "not there yet" becomes a fault. A TOO_EARLY read counts once per
    // index, and only once the index is graceNs past its media timestamp: a
    // reader at the head finds nothing until the next grain is due, which is
    // normal; while the rate is unknown, TOO_EARLY does not count. A stall
    // counts once when stallTimeoutNs pass without data, and again only after
    // data moved or the flow was opened. Unless this was called, the reader
    // wrappers derive both on every open, as mxl-reader does.
    mxlStatus mxlMonitoringSetFlowTiming(mxlMonitoringFlow flow, uint64_t graceNs, uint64_t stallTimeoutNs);

    // Each attempt to reach the flow: MXL_MONITORING_ACCESS_OPEN on success,
    // the reason otherwise.
    void mxlMonitoringRecordAccess(mxlMonitoringFlow flow, mxlMonitoringAccess access, mxlStatus status);

    // The result of one libmxl read. index is the grain read, or the head
    // index of the samples read; count the grains or samples an OK read
    // covers. Every other outcome counts occurrences, not units. A read that
    // fails for a reason other than timing leaves the reader unusable, so the
    // flow's access follows (PERMISSION_DENIED or OTHER) until data moves or
    // the flow is opened again.
    void mxlMonitoringRecordRead(mxlMonitoringFlow flow, mxlStatus status, uint64_t index, uint64_t count);

    // Data read that turned out wrong. Counted as INVALID_CONTENT instead of
    // OK, never as both.
    void mxlMonitoringRecordReadContent(mxlMonitoringFlow flow, mxlMonitoringContent issue, uint64_t index, uint64_t count);

    // Grains or samples jumped over after TOO_LATE to reach the head again.
    void mxlMonitoringRecordSkipped(mxlMonitoringFlow flow, uint64_t count);

    // When index was committed, for a reader: the flow's lastWriteTime while
    // index is the head, the only time libmxl says when a particular grain was
    // written. The reader wrappers call it. A writer's commit latency comes
    // from mxlMonitoringRecordWrite.
    void mxlMonitoringRecordCommit(mxlMonitoringFlow flow, uint64_t index, uint64_t commitTimeNs);

    // The result of committing a grain or samples, or of failing to open them
    // for writing. An OK commit counts its units and observes the commit
    // latency at the time of the call; any other outcome counts one
    // occurrence. Losses and access follow as for reads.
    void mxlMonitoringRecordWrite(mxlMonitoringFlow flow, mxlStatus status, uint64_t index, uint64_t count);

    // The two write outcomes no libmxl status carries, counted in units:
    // INPUT_LATE for the grains or samples whose input missed their deadline,
    // OUT_OF_SPACE from ENOSPC (count 1 for one failed attempt). Any other
    // outcome is ignored. Access does not change.
    void mxlMonitoringRecordWriteOutcome(mxlMonitoringFlow flow, mxlMonitoringWrite outcome, mxlStatus status, uint64_t index, uint64_t count);

    // Input found wrong before writing. Counted as INVALID_INPUT.
    void mxlMonitoringRecordWriteContent(mxlMonitoringFlow flow, mxlMonitoringContent issue, uint64_t index, uint64_t count);

    // When the input for index arrived, for a writer's input latency.
    void mxlMonitoringRecordInput(mxlMonitoringFlow flow, uint64_t index, uint64_t arrivalNs);

    // When data last moved on the flow, or it was opened. 0 before either.
    uint64_t mxlMonitoringFlowLastProgress(mxlMonitoringFlow flow);

    // Upper bounds in seconds: 0.001 0.002 0.005 0.01 0.02 0.04 0.08 0.16
    // 0.32 0.64 1.28, then one bucket for everything above.
#define MXL_MONITORING_LATENCY_BUCKETS 12

    typedef struct mxlMonitoringHistogram_t
    {
        uint64_t buckets[MXL_MONITORING_LATENCY_BUCKETS]; // per bucket, not cumulative
        uint64_t count;
        // Negative latencies, from clocks apart between nodes, count as 0.
        uint64_t sumNs;
    } mxlMonitoringHistogram;

    uint64_t mxlMonitoringLatencyBound(size_t bucket);

#define MXL_MONITORING_TEXT 256

    typedef struct mxlMonitoringFlowSnapshot_t
    {
        uint32_t version; // 1
        uint32_t size;    // sizeof(mxlMonitoringFlowSnapshot)

        char flowId[MXL_MONITORING_ID];
        // The libmxl flow the handle reads or writes; "" when disconnected.
        char target[MXL_MONITORING_ID];
        mxlMonitoringRole role;
        mxlMonitoringUnit unit;
        mxlDataFormat format;
        mxlRational rate;
        char mediaType[64];

        mxlMonitoringAccess access;
        // Failed attempts, by reason; the STARTING and OPEN slots stay 0.
        uint64_t accessFailures[MXL_MONITORING_ACCESS_COUNT];
        uint64_t opens;
        // Times an open flow went away.
        uint64_t losses;

        // OK, INVALID_CONTENT, INVALID_INPUT, INPUT_LATE, OUT_OF_SPACE and
        // content in units of `unit`; every other outcome in occurrences.
        uint64_t reads[MXL_MONITORING_READ_COUNT];
        uint64_t writes[MXL_MONITORING_WRITE_COUNT];
        uint64_t content[MXL_MONITORING_CONTENT_COUNT];
        uint64_t skipped;
        uint64_t stalls;

        mxlStatus lastErrorStatus;
        uint64_t lastErrorIndex;
        uint64_t lastErrorTimeNs; // 0 before any error
        char lastErrorDetail[MXL_MONITORING_TEXT];
        // How the flow differs from its expectation, "" when it does not.
        char formatMismatch[MXL_MONITORING_TEXT];

        // Reader: read time minus the media timestamp of what was read.
        // Latencies need the rate; nothing is observed while it is unknown.
        mxlMonitoringHistogram arrivalLatency;
        // Commit time minus the media timestamp of what was committed.
        mxlMonitoringHistogram commitLatency;
        // Writer: input arrival minus the media timestamp it is written at.
        mxlMonitoringHistogram inputLatency;

        uint64_t lastIndex;      // last index read or written
        uint64_t lastDataTimeNs; // when; 0 before the first
        // Clock minus the media timestamp of lastIndex, at lastDataTimeNs; 0
        // while the rate is unknown.
        int64_t ageNs;
        // Open, and data moved within the last second.
        bool delivering;
    } mxlMonitoringFlowSnapshot;

    // Fills up to *count snapshots, in the order the flows were added, and
    // sets *count to the number of flows.
    mxlStatus mxlMonitoringSnapshot(mxlMonitoring monitoring, mxlMonitoringFlowSnapshot* snapshots, size_t* count);

    // A condition is one way a flow is failing: one access failure, one read
    // or write outcome, or, for INVALID_CONTENT and INVALID_INPUT, one
    // content issue, so wrong format and illegal values notify separately.
    typedef enum mxlMonitoringConditionKind
    {
        MXL_MONITORING_CONDITION_ACCESS,
        MXL_MONITORING_CONDITION_READ,
        MXL_MONITORING_CONDITION_WRITE,
    } mxlMonitoringConditionKind;

    // What the events of one condition say, for a caller recording by hand:
    // kind and outcome name the condition as in mxlMonitoringEvent (outcome
    // an mxlMonitoringAccess, -Read or -Write value), content the issue for
    // INVALID_CONTENT and INVALID_INPUT. The reader wrappers describe what
    // they record themselves. Not for a hot path: it takes a lock and copies
    // the text.
    mxlStatus mxlMonitoringDescribe(mxlMonitoringFlow flow, mxlMonitoringConditionKind kind, int outcome, mxlMonitoringContent content,
        char const* detail);

    typedef enum mxlMonitoringPhase
    {
        MXL_MONITORING_PHASE_STARTED,    // first occurrence
        // Once per persistIntervalNs while it recurs, or while the flow it
        // concerns delivers nothing and is not disconnected.
        MXL_MONITORING_PHASE_PERSISTING,
        // Access: the flow is open again, or disconnected on purpose. Read
        // and write: see recoverDelayNs.
        MXL_MONITORING_PHASE_RECOVERED,
    } mxlMonitoringPhase;

    typedef struct mxlMonitoringEvent_t
    {
        uint32_t version; // 1
        uint32_t size;    // sizeof(mxlMonitoringEvent)

        uint64_t timeNs;
        char flowId[MXL_MONITORING_ID];
        mxlMonitoringRole role;
        mxlMonitoringConditionKind kind;
        mxlMonitoringPhase phase;
        union
        {
            mxlMonitoringAccess access;
            mxlMonitoringRead read;
            mxlMonitoringWrite write;
        };
        // For INVALID_CONTENT and INVALID_INPUT; MXL_MONITORING_CONTENT_COUNT
        // otherwise.
        mxlMonitoringContent content;

        mxlStatus status; // the last libmxl status of the condition
        uint64_t index;   // the last affected grain or sample
        uint64_t occurrences; // since the previous event of this condition
        uint64_t total;       // since STARTED
        uint64_t durationNs;  // since STARTED
        char detail[MXL_MONITORING_TEXT];
    } mxlMonitoringEvent;

    typedef void (*mxlMonitoringEventFn)(mxlMonitoringEvent const* event, void* user);

    // Every subscriber receives every event, on the thread calling
    // mxlMonitoringPoll and under its lock: a subscriber must not call back
    // into the context, and a slow one holds up mxlMonitoringRemoveFlow and
    // mxlMonitoringSubscribe, though never recording.
    mxlStatus mxlMonitoringSubscribe(mxlMonitoring monitoring, mxlMonitoringEventFn fn, void* user);

    // Advances every condition to now and delivers the resulting events. Call
    // from one thread, more often than recoverDelayNs; a notification is late
    // by at most the interval between polls.
    mxlStatus mxlMonitoringPoll(mxlMonitoring monitoring);

#ifdef __cplusplus
}
#endif
