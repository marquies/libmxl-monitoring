#include "mxl-monitoring/writer.h"
#include <cerrno>
#include <format>
#include <string>
#include <sys/statvfs.h>
#include <mxl/flowinfo.h>
#include "Flow.hpp"

namespace mxl::monitoring
{
    namespace
    {
        // Whether a failed create ran out of space. libmxl has no status for
        // it: the errno of the failing call may survive to the caller, and a
        // full filesystem shows no blocks left.
        bool outOfSpace(int error, std::string const& domain)
        {
            if (error == ENOSPC)
            {
                return true;
            }
            struct statvfs fs{};
            return !domain.empty() && ::statvfs(domain.c_str(), &fs) == 0 && fs.f_bavail == 0;
        }
    }
}

using namespace mxl::monitoring;

extern "C"
{
    mxlStatus mxlMonitoringCreateFlowWriter(mxlMonitoringFlow flow, mxlInstance instance, char const* flowDef, char const* options,
        mxlFlowWriter* writer, mxlFlowConfigInfo* configInfo, bool* created)
    {
        auto config = mxlFlowConfigInfo{};
        errno = 0;
        auto const status = mxlCreateFlowWriter(instance, flowDef, options, writer, &config, created);
        auto const error = errno;
        if (configInfo != nullptr)
        {
            *configInfo = config;
        }
        auto* const f = toFlow(flow);
        if (f == nullptr)
        {
            return status;
        }
        if (status != MXL_STATUS_OK)
        {
            auto const domain = domainOf(*f);
            auto access = mxlMonitoringClassifyAccess(status);
            auto detail = std::format("cannot create flow {}: status {}", f->id, static_cast<int>(status));
            if (auto const domainDetail = domainUnavailable(domain); !domainDetail.empty())
            {
                access = MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE;
                detail = domainDetail;
            }
            else if (outOfSpace(error, domain))
            {
                access = MXL_MONITORING_ACCESS_OTHER;
                detail = std::format("no space left in MXL domain {}", domain);
                mxlMonitoringRecordWriteOutcome(flow, MXL_MONITORING_WRITE_OUT_OF_SPACE, status, MXL_UNDEFINED_INDEX, 1);
            }
            else if (access == MXL_MONITORING_ACCESS_PERMISSION_DENIED)
            {
                detail = std::format("permission denied creating flow {}", f->id);
            }
            else if (access == MXL_MONITORING_ACCESS_FLOW_INVALID)
            {
                // Replaced data is a loss only under a writer already open.
                access = MXL_MONITORING_ACCESS_OTHER;
            }
            f->describeAccess(access, std::move(detail));
            mxlMonitoringRecordAccess(flow, access, status);
            return status;
        }

        // A definition the library cannot parse leaves the flow without its
        // declared format; it is still open and counts what it writes.
        mxlMonitoringSetDefinition(flow, flowDef);
        auto const& common = config.common;
        auto const continuous = common.format == MXL_DATA_FORMAT_AUDIO;
        auto mediaType = std::string{};
        {
            auto const lock = std::lock_guard{f->mutex};
            mediaType = f->declared.mediaType;
        }
        auto format = mxlMonitoringFlowFormat{.version = 1,
            .size = sizeof(mxlMonitoringFlowFormat),
            .format = static_cast<mxlDataFormat>(common.format),
            .unit = continuous ? MXL_MONITORING_UNIT_SAMPLE : MXL_MONITORING_UNIT_GRAIN,
            .rate = common.grainRate,
            .mediaType = mediaType.c_str()};
        mxlMonitoringSetFlowFormat(flow, &format);
        mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
        return MXL_STATUS_OK;
    }

    mxlStatus mxlMonitoringFlowWriterOpenGrain(mxlMonitoringFlow flow, mxlFlowWriter writer, uint64_t index, mxlGrainInfo* grainInfo,
        uint8_t** payload)
    {
        auto const status = mxlFlowWriterOpenGrain(writer, index, grainInfo, payload);
        if (status != MXL_STATUS_OK)
        {
            mxlMonitoringRecordWrite(flow, status, index, 1);
        }
        return status;
    }

    mxlStatus mxlMonitoringFlowWriterCommitGrain(mxlMonitoringFlow flow, mxlFlowWriter writer, mxlGrainInfo const* grain)
    {
        auto const status = mxlFlowWriterCommitGrain(writer, grain);
        if (grain == nullptr)
        {
            return status;
        }
        if (status != MXL_STATUS_OK)
        {
            mxlMonitoringRecordWrite(flow, status, grain->index, 1);
        }
        else if ((grain->flags & MXL_GRAIN_FLAG_INVALID) == 0 && grain->validSlices == grain->totalSlices)
        {
            mxlMonitoringRecordWrite(flow, status, grain->index, 1);
        }
        return status;
    }

    mxlStatus mxlMonitoringFlowWriterOpenSamples(mxlMonitoringFlow flow, mxlFlowWriter writer, uint64_t index, size_t count,
        mxlMutableWrappedMultiBufferSlice* payloadBuffersSlices)
    {
        auto const status = mxlFlowWriterOpenSamples(writer, index, count, payloadBuffersSlices);
        if (auto* const f = toFlow(flow); f != nullptr)
        {
            f->samplesIndex.store(index, std::memory_order_relaxed);
            f->samplesCount.store(count, std::memory_order_relaxed);
        }
        if (status != MXL_STATUS_OK)
        {
            mxlMonitoringRecordWrite(flow, status, index, count);
        }
        return status;
    }

    mxlStatus mxlMonitoringFlowWriterCommitSamples(mxlMonitoringFlow flow, mxlFlowWriter writer)
    {
        auto const status = mxlFlowWriterCommitSamples(writer);
        if (auto* const f = toFlow(flow); f != nullptr)
        {
            mxlMonitoringRecordWrite(flow, status, f->samplesIndex.load(std::memory_order_relaxed), f->samplesCount.load(std::memory_order_relaxed));
        }
        return status;
    }
}
