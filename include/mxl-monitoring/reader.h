// libmxl reader calls that record their own outcome. Each wrapper takes the
// arguments of the libmxl function it is named after, plus the flow it
// records on, calls it, records the result on the flow and returns the libmxl
// status unchanged. A grain or a batch of samples read successfully passes
// the content checks of content.h before it counts as OK.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include "monitoring.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // mxlCreateInstance. A NULL result counts as DOMAIN_UNAVAILABLE. The
    // domain is kept on the flow, for telling a permission failure apart.
    mxlInstance mxlMonitoringCreateInstance(mxlMonitoringFlow flow, char const* domain, char const* options);

    // mxlCreateFlowReader for the flow's id, then its info and definition:
    // the flow counts as open only once all three are read, and a reader
    // whose definition cannot be read is released again. libmxl reports a
    // reader it could not open for want of permission as an unknown error,
    // so on failure the flow's files are checked for permission too.
    mxlStatus mxlMonitoringCreateFlowReader(mxlMonitoringFlow flow, mxlInstance instance, char const* options, mxlFlowReader* reader);

    mxlStatus mxlMonitoringFlowReaderGetGrain(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index, uint64_t timeoutNs,
        mxlGrainInfo* grain, uint8_t** payload);
    mxlStatus mxlMonitoringFlowReaderGetGrainNonBlocking(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index, mxlGrainInfo* grain,
        uint8_t** payload);
    mxlStatus mxlMonitoringFlowReaderGetGrainSlice(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index, uint16_t minValidSlices,
        uint64_t timeoutNs, mxlGrainInfo* grain, uint8_t** payload);
    mxlStatus mxlMonitoringFlowReaderGetGrainSliceNonBlocking(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index,
        uint16_t minValidSlices, mxlGrainInfo* grain, uint8_t** payload);

    mxlStatus mxlMonitoringFlowReaderGetSamples(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index, size_t count, uint64_t timeoutNs,
        mxlWrappedMultiBufferSlice* samples);
    mxlStatus mxlMonitoringFlowReaderGetSamplesNonBlocking(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index, size_t count,
        mxlWrappedMultiBufferSlice* samples);

#ifdef __cplusplus
}
#endif
