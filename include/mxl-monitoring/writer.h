// libmxl writer calls that record their own outcome. Each wrapper takes the
// arguments of the libmxl function it is named after, plus the flow it
// records on, calls it, records the result on the flow and returns the libmxl
// status unchanged. What only the writer knows, input that came too late or
// was wrong, it records with mxlMonitoringRecordWriteOutcome and
// mxlMonitoringRecordWriteContent. The instance comes from
// mxlMonitoringCreateInstance in reader.h, which keeps the domain on the flow.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include "monitoring.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // mxlCreateFlowWriter. On success the flow is open, with the format and
    // definition of flowDef. A failure counts as an access failure: a domain
    // that is missing or cannot be read as DOMAIN_UNAVAILABLE, otherwise as
    // libmxl's status classifies. libmxl has no status for a full domain, so
    // a failure with errno ENOSPC, or with no space left on the domain's
    // filesystem, also counts as an OUT_OF_SPACE write.
    mxlStatus mxlMonitoringCreateFlowWriter(mxlMonitoringFlow flow, mxlInstance instance, char const* flowDef, char const* options,
        mxlFlowWriter* writer, mxlFlowConfigInfo* configInfo, bool* created);

    // mxlFlowWriterOpenGrain. A failure counts as a write outcome for index.
    mxlStatus mxlMonitoringFlowWriterOpenGrain(mxlMonitoringFlow flow, mxlFlowWriter writer, uint64_t index, mxlGrainInfo* grainInfo,
        uint8_t** payload);

    // mxlFlowWriterCommitGrain. A grain counts as written once, on the commit
    // that makes all its slices valid, and its commit latency is observed
    // then; a commit of some slices counts nothing yet. A grain flagged
    // MXL_GRAIN_FLAG_INVALID does not count as written: the writer records
    // why it wrote one, such as INPUT_LATE for a gap it filled. A failure
    // counts as a write outcome for the grain's index.
    mxlStatus mxlMonitoringFlowWriterCommitGrain(mxlMonitoringFlow flow, mxlFlowWriter writer, mxlGrainInfo const* grain);

    // mxlFlowWriterOpenSamples. The range is kept for the commit. A failure
    // counts as a write outcome for the range.
    mxlStatus mxlMonitoringFlowWriterOpenSamples(mxlMonitoringFlow flow, mxlFlowWriter writer, uint64_t index, size_t count,
        mxlMutableWrappedMultiBufferSlice* payloadBuffersSlices);

    // mxlFlowWriterCommitSamples. Counts the samples of the range opened last,
    // as written or as the failure's outcome.
    mxlStatus mxlMonitoringFlowWriterCommitSamples(mxlMonitoringFlow flow, mxlFlowWriter writer);

#ifdef __cplusplus
}
#endif
