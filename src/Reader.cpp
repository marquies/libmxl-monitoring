#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <vector>
#include <mxl/flowinfo.h>
#include <mxl/time.h>
#include "mxl-monitoring/reader.h"
#include "Flow.hpp"

namespace mxl::monitoring
{
    namespace
    {
        std::uint64_t periodNs(mxlRational const& rate)
        {
            if (rate.numerator <= 0 || rate.denominator <= 0)
            {
                return NsPerSecond / 50;
            }
            return NsPerSecond * static_cast<std::uint64_t>(rate.denominator) / static_cast<std::uint64_t>(rate.numerator);
        }

        // Releases a reader that was opened but cannot be used, and records
        // why: nothing can be checked against a flow that is not described.
        mxlStatus abandon(Flow& flow, mxlInstance instance, mxlFlowReader* reader, mxlStatus status, std::string detail)
        {
            mxlReleaseFlowReader(instance, *reader);
            *reader = nullptr;
            flow.describeAccess(MXL_MONITORING_ACCESS_OTHER, std::move(detail));
            mxlMonitoringRecordAccess(toHandle(&flow), MXL_MONITORING_ACCESS_OTHER, status);
            return status;
        }

        mxlStatus readDefinition(mxlInstance instance, std::string const& id, std::string& out)
        {
            auto buffer = std::vector<char>(8192);
            auto size = buffer.size();
            auto status = mxlGetFlowDef(instance, id.c_str(), buffer.data(), &size);
            if (status == MXL_ERR_INVALID_ARG && size > buffer.size())
            {
                buffer.resize(size);
                status = mxlGetFlowDef(instance, id.c_str(), buffer.data(), &size);
            }
            if (status == MXL_STATUS_OK)
            {
                out = buffer.data();
            }
            return status;
        }

        // The flow was committed when it was the head, the only time libmxl
        // says when a particular index was written.
        void observeCommit(Flow& flow, mxlFlowReader reader, std::uint64_t index)
        {
            auto runtime = mxlFlowRuntimeInfo{};
            if (mxlFlowReaderGetRuntimeInfo(reader, &runtime) == MXL_STATUS_OK && runtime.headIndex == index && runtime.lastWriteTime != 0)
            {
                mxlMonitoringRecordCommit(toHandle(&flow), index, runtime.lastWriteTime);
            }
        }

        void describeFailure(Flow& flow, mxlStatus status)
        {
            switch (mxlMonitoringClassifyRead(status))
            {
                case MXL_MONITORING_READ_TOO_EARLY:
                case MXL_MONITORING_READ_TOO_LATE:  return;
                default:                            break;
            }
            if (isLoss(status))
            {
                flow.describeAccess(mxlMonitoringClassifyAccess(status), std::format("flow {} went away", flow.targetId()));
            }
            else if (status == MXL_ERR_PERMISSION_DENIED)
            {
                flow.describeFailure("permission denied reading the flow");
            }
            else
            {
                flow.describeFailure(std::format("read failed: status {}", static_cast<int>(status)));
            }
        }

        void recordIssue(Flow& flow, mxlMonitoringContent issue, char const* detail, std::uint64_t index, std::uint64_t count)
        {
            if (issue == MXL_MONITORING_CONTENT_ILLEGAL_VALUES && *detail != '\0')
            {
                flow.describeContent(issue, std::format("illegal sample value: {}", detail));
            }
            mxlMonitoringRecordReadContent(toHandle(&flow), issue, index, count);
        }

        mxlStatus recordGrain(Flow& flow, mxlFlowReader reader, mxlStatus status, std::uint64_t index, mxlGrainInfo const* grain,
            std::uint8_t const* payload)
        {
            if (status != MXL_STATUS_OK)
            {
                describeFailure(flow, status);
                mxlMonitoringRecordRead(toHandle(&flow), status, index, 1);
                return status;
            }
            auto issue = mxlMonitoringContent{};
            auto detail = std::array<char, MXL_MONITORING_TEXT>{};
            if (mxlMonitoringCheckGrain(&flow.declared, flow.mismatch.load(std::memory_order_relaxed), flow.checkLegal.load(std::memory_order_relaxed),
                    grain, index, payload, &issue, detail.data(), detail.size()))
            {
                recordIssue(flow, issue, detail.data(), index, 1);
            }
            else
            {
                mxlMonitoringRecordRead(toHandle(&flow), status, index, 1);
            }
            observeCommit(flow, reader, index);
            return status;
        }

        mxlStatus recordSamples(Flow& flow, mxlFlowReader reader, mxlStatus status, std::uint64_t index, std::size_t count,
            mxlWrappedMultiBufferSlice const* samples)
        {
            if (status != MXL_STATUS_OK)
            {
                describeFailure(flow, status);
                mxlMonitoringRecordRead(toHandle(&flow), status, index, count);
                return status;
            }
            auto issue = mxlMonitoringContent{};
            auto detail = std::array<char, MXL_MONITORING_TEXT>{};
            if (mxlMonitoringCheckSamples(&flow.declared, flow.mismatch.load(std::memory_order_relaxed),
                    flow.checkLegal.load(std::memory_order_relaxed), samples, &issue, detail.data(), detail.size()))
            {
                recordIssue(flow, issue, detail.data(), index, count);
            }
            else
            {
                mxlMonitoringRecordRead(toHandle(&flow), status, index, count);
            }
            observeCommit(flow, reader, index);
            return status;
        }
    }
}

using namespace mxl::monitoring;

extern "C"
{
    mxlInstance mxlMonitoringCreateInstance(mxlMonitoringFlow flow, char const* domain, char const* options)
    {
        auto const instance = mxlCreateInstance(domain, options);
        auto* const f = toFlow(flow);
        if (f == nullptr || domain == nullptr)
        {
            return instance;
        }
        {
            auto const lock = std::lock_guard{f->mutex};
            f->domain = domain;
        }
        if (instance == nullptr)
        {
            // The volume may not be mounted yet, or nothing has created the
            // domain: tell the two apart for whoever reads the detail.
            auto const detail = domainUnavailable(domain);
            f->describeAccess(MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE, detail.empty() ? std::format("cannot open MXL domain {}", domain) : detail);
            mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE, MXL_ERR_UNKNOWN);
        }
        return instance;
    }

    mxlStatus mxlMonitoringCreateFlowReader(mxlMonitoringFlow flow, mxlInstance instance, char const* options, mxlFlowReader* reader)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr || reader == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const target = f->targetId();
        if (target.empty())
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto status = mxlCreateFlowReader(instance, target.c_str(), options, reader);
        if (status != MXL_STATUS_OK)
        {
            auto access = mxlMonitoringClassifyAccess(status);
            // Replaced data is a loss only under a reader already open.
            if (access == MXL_MONITORING_ACCESS_FLOW_INVALID)
            {
                access = MXL_MONITORING_ACCESS_OTHER;
            }
            auto const domain = domainOf(*f);
            auto const domainDetail = domainUnavailable(domain);
            if (!domainDetail.empty())
            {
                access = MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE;
            }
            else if (access != MXL_MONITORING_ACCESS_FLOW_NOT_FOUND && access != MXL_MONITORING_ACCESS_PERMISSION_DENIED &&
                     permissionDenied(domain, target))
            {
                access = MXL_MONITORING_ACCESS_PERMISSION_DENIED;
            }
            f->describeAccess(access,
                access == MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE ? domainDetail
                : access == MXL_MONITORING_ACCESS_FLOW_NOT_FOUND   ? std::format("flow {} not found in the domain", target)
                : access == MXL_MONITORING_ACCESS_PERMISSION_DENIED
                    ? std::format("permission denied opening flow {}", target)
                    : std::format("cannot open flow {}: status {}", target, static_cast<int>(status)));
            mxlMonitoringRecordAccess(flow, access, status);
            return status;
        }

        auto info = mxlFlowInfo{};
        if (status = mxlFlowReaderGetInfo(*reader, &info); status != MXL_STATUS_OK)
        {
            return abandon(*f, instance, reader, status, std::format("cannot read the info of flow {}: status {}", target, static_cast<int>(status)));
        }
        auto definition = std::string{};
        if (status = readDefinition(instance, target, definition); status != MXL_STATUS_OK)
        {
            return abandon(*f, instance, reader, status, std::format("cannot read the definition of flow {}", target));
        }
        if (status = mxlMonitoringSetDefinition(flow, definition.c_str()); status != MXL_STATUS_OK)
        {
            return abandon(*f, instance, reader, status, std::format("cannot read the definition of flow {}", target));
        }

        auto const& common = info.config.common;
        auto const continuous = common.format == MXL_DATA_FORMAT_AUDIO;
        auto format = mxlMonitoringFlowFormat{.version = 1,
            .size = sizeof(mxlMonitoringFlowFormat),
            .format = static_cast<mxlDataFormat>(common.format),
            .unit = continuous ? MXL_MONITORING_UNIT_SAMPLE : MXL_MONITORING_UNIT_GRAIN,
            .rate = common.grainRate,
            .mediaType = f->declared.mediaType};
        mxlMonitoringSetFlowFormat(flow, &format);

        // As mxl-reader times it, from the flow as it is on this open: data
        // may land one read's span after its timestamp, or for samples the
        // writer's commit batch if larger, plus 50 ms for scheduling, before
        // it counts as too early. A read spans a grain, or 10 ms of samples
        // or the writer's sync batch if larger. A stall is four read spans
        // without data, at least 100 ms, so a grain committed late is a slow
        // grain rather than a stall. A caller that reads differently sets its
        // own with mxlMonitoringSetFlowTiming.
        if (!f->timingSet.load(std::memory_order_relaxed))
        {
            auto const period = periodNs(common.grainRate);
            auto readNs = period;
            auto commitNs = std::uint64_t{0};
            if (continuous)
            {
                auto const perHundredth =
                    static_cast<std::uint64_t>(common.grainRate.numerator / std::max<std::int64_t>(1, common.grainRate.denominator) / 100);
                readNs = std::max<std::uint64_t>({1, perHundredth, common.maxSyncBatchSizeHint}) * period;
                commitNs = common.maxCommitBatchSizeHint * period;
            }
            f->graceNs.store(std::max(readNs, commitNs) + NsPerSecond / 20, std::memory_order_relaxed);
            f->stallTimeoutNs.store(std::max<std::uint64_t>(4 * readNs, NsPerSecond / 10), std::memory_order_relaxed);
        }

        {
            auto const lock = std::lock_guard{f->mutex};
            if (!f->formatMismatch.empty())
            {
                f->lastErrorDetail = "format differs from what was expected: " + f->formatMismatch;
                for (auto const issue : {MXL_MONITORING_CONTENT_WRONG_FORMAT, MXL_MONITORING_CONTENT_WRONG_RESOLUTION, MXL_MONITORING_CONTENT_WRONG_RATE})
                {
                    f->contentDetail[issue] = f->lastErrorDetail;
                }
            }
        }
        mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
        return MXL_STATUS_OK;
    }

    mxlStatus mxlMonitoringFlowReaderGetGrain(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index, uint64_t timeoutNs,
        mxlGrainInfo* grain, uint8_t** payload)
    {
        auto const status = mxlFlowReaderGetGrain(reader, index, timeoutNs, grain, payload);
        auto* const f = toFlow(flow);
        return f != nullptr ? recordGrain(*f, reader, status, index, grain, payload != nullptr ? *payload : nullptr) : status;
    }

    mxlStatus mxlMonitoringFlowReaderGetGrainNonBlocking(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index, mxlGrainInfo* grain,
        uint8_t** payload)
    {
        auto const status = mxlFlowReaderGetGrainNonBlocking(reader, index, grain, payload);
        auto* const f = toFlow(flow);
        return f != nullptr ? recordGrain(*f, reader, status, index, grain, payload != nullptr ? *payload : nullptr) : status;
    }

    mxlStatus mxlMonitoringFlowReaderGetGrainSlice(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index, uint16_t minValidSlices,
        uint64_t timeoutNs, mxlGrainInfo* grain, uint8_t** payload)
    {
        auto const status = mxlFlowReaderGetGrainSlice(reader, index, minValidSlices, timeoutNs, grain, payload);
        auto* const f = toFlow(flow);
        return f != nullptr ? recordGrain(*f, reader, status, index, grain, payload != nullptr ? *payload : nullptr) : status;
    }

    mxlStatus mxlMonitoringFlowReaderGetGrainSliceNonBlocking(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index,
        uint16_t minValidSlices, mxlGrainInfo* grain, uint8_t** payload)
    {
        auto const status = mxlFlowReaderGetGrainSliceNonBlocking(reader, index, minValidSlices, grain, payload);
        auto* const f = toFlow(flow);
        return f != nullptr ? recordGrain(*f, reader, status, index, grain, payload != nullptr ? *payload : nullptr) : status;
    }

    mxlStatus mxlMonitoringFlowReaderGetSamples(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index, size_t count, uint64_t timeoutNs,
        mxlWrappedMultiBufferSlice* samples)
    {
        auto const status = mxlFlowReaderGetSamples(reader, index, count, timeoutNs, samples);
        auto* const f = toFlow(flow);
        return f != nullptr ? recordSamples(*f, reader, status, index, count, samples) : status;
    }

    mxlStatus mxlMonitoringFlowReaderGetSamplesNonBlocking(mxlMonitoringFlow flow, mxlFlowReader reader, uint64_t index, size_t count,
        mxlWrappedMultiBufferSlice* samples)
    {
        auto const status = mxlFlowReaderGetSamplesNonBlocking(reader, index, count, samples);
        auto* const f = toFlow(flow);
        return f != nullptr ? recordSamples(*f, reader, status, index, count, samples) : status;
    }
}
