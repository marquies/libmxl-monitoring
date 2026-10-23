// Whether a flow and its grains or samples are what they should be: what a
// flow declares in its definition, what was expected of it, and the checks a
// grain or a batch of samples passes through. The functions without a flow
// are pure and touch no libmxl state.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mxl/flow.h>
#include <mxl/rational.h>
#include "monitoring.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // What a flow declares about itself in its flow definition.
    typedef struct mxlMonitoringDeclared_t
    {
        char mediaType[64];
        int32_t width;
        int32_t height;
        bool interlaced;
        mxlRational rate; // grain_rate; {0, 0} when absent
        int32_t sampleRate;
        int32_t channels;
    } mxlMonitoringDeclared;

    // MXL_ERR_INVALID_ARG when the text is not a JSON object.
    mxlStatus mxlMonitoringParseDefinition(char const* flowDef, mxlMonitoringDeclared* declared);

    // What the flow should carry. A field left 0, {0, 0} or NULL is not
    // checked.
    typedef struct mxlMonitoringExpectation_t
    {
        uint32_t version; // 1
        uint32_t size;    // sizeof(mxlMonitoringExpectation)

        char const* mediaType;
        int32_t width;
        int32_t height;
        mxlRational rate;
        int32_t sampleRate;
        int32_t channels;
        // Check every sample against the legal range of its format. Off
        // unless asked: a test pattern may carry reserved codes on purpose,
        // and a check that always fires hides the faults it is there to find.
        bool checkLegal;
    } mxlMonitoringExpectation;

    // How declared differs from expected: a bit (1u << issue) for each of
    // WRONG_FORMAT, WRONG_RESOLUTION and WRONG_RATE, 0 when it does not
    // differ. message, when not NULL, receives the differences as text.
    uint32_t mxlMonitoringMismatch(mxlMonitoringDeclared const* declared, mxlMonitoringExpectation const* expected, char* message,
        size_t messageSize);

    // The one issue a grain read at index has, worst first, so a grain counts
    // once whatever else is wrong with it. Returns false when there is none.
    // mismatch is what mxlMonitoringMismatch returned for the flow. detail,
    // when not NULL, receives where an illegal value sits.
    bool mxlMonitoringCheckGrain(mxlMonitoringDeclared const* declared, uint32_t mismatch, bool checkLegal, mxlGrainInfo const* grain,
        uint64_t index, uint8_t const* payload, mxlMonitoringContent* issue, char* detail, size_t detailSize);

    // The same for a batch of samples. The legal range is checked for
    // audio/float32: finite and within -1..1.
    bool mxlMonitoringCheckSamples(mxlMonitoringDeclared const* declared, uint32_t mismatch, bool checkLegal,
        mxlWrappedMultiBufferSlice const* samples, mxlMonitoringContent* issue, char* detail, size_t detailSize);

    // What the flow should carry, for the checks the reader wrappers run.
    mxlStatus mxlMonitoringSetExpectation(mxlMonitoringFlow flow, mxlMonitoringExpectation const* expected);

    // The flow's definition, once opened: parsed, compared with the
    // expectation, and kept for the checks. The reader wrappers call it. Call
    // it from the thread recording for the flow: the checks read the parsed
    // definition without a lock.
    mxlStatus mxlMonitoringSetDefinition(mxlMonitoringFlow flow, char const* flowDef);

    // What the flow declared, after mxlMonitoringSetDefinition.
    mxlStatus mxlMonitoringGetDeclared(mxlMonitoringFlow flow, mxlMonitoringDeclared* declared);

#ifdef __cplusplus
}
#endif
