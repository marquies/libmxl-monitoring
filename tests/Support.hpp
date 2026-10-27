#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "mxl-monitoring/monitoring.h"

namespace mxl::monitoring::tests
{
    constexpr std::uint64_t Ms = 1'000'000ULL;
    constexpr std::uint64_t Second = 1'000'000'000ULL;

    inline char const* const FlowId = "d4d00000-0000-0000-0000-000000000001";

    // Small flows for the wrapper tests against a real domain.
    inline std::string videoDefinition(char const* id)
    {
        return std::string{R"({"id": ")"} + id + R"(", "description": "test", "label": "test",
            "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["libmxl-monitoring-tests:Video"]},
            "format": "urn:x-nmos:format:video", "parents": [], "media_type": "video/v210",
            "grain_rate": {"numerator": 50, "denominator": 1},
            "frame_width": 96, "frame_height": 54, "interlace_mode": "progressive", "colorspace": "BT709",
            "components": [{"name": "Y", "width": 96, "height": 54, "bit_depth": 10},
                           {"name": "Cb", "width": 48, "height": 54, "bit_depth": 10},
                           {"name": "Cr", "width": 48, "height": 54, "bit_depth": 10}]})";
    }

    inline std::string audioDefinition(char const* id)
    {
        return std::string{R"({"id": ")"} + id + R"(", "description": "test", "label": "test",
            "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["libmxl-monitoring-tests:Audio"]},
            "format": "urn:x-nmos:format:audio", "parents": [], "media_type": "audio/float32",
            "sample_rate": {"numerator": 48000}, "channel_count": 2, "bit_depth": 32})";
    }

    // A context on a clock the test moves by hand, with every event kept.
    struct Context
    {
        explicit Context(std::uint64_t start = 1'000 * Second)
            : now{start}
        {
            auto config = mxlMonitoringConfig{};
            config.version = 1;
            config.size = sizeof(config);
            config.persistIntervalNs = 60 * Second;
            config.recoverDelayNs = Second;
            config.now = [](void* user) { return *static_cast<std::uint64_t*>(user); };
            config.nowUser = &now;
            REQUIRE(mxlMonitoringCreate(&config, &monitoring) == MXL_STATUS_OK);
            REQUIRE(mxlMonitoringSubscribe(monitoring,
                        [](mxlMonitoringEvent const* event, void* user) { static_cast<Context*>(user)->events.push_back(*event); },
                        this) == MXL_STATUS_OK);
        }

        ~Context()
        {
            mxlMonitoringDestroy(monitoring);
        }

        Context(Context const&) = delete;
        Context& operator=(Context const&) = delete;

        mxlMonitoringFlow add(mxlMonitoringRole role = MXL_MONITORING_ROLE_READER, char const* id = FlowId)
        {
            auto flow = mxlMonitoringFlow{};
            REQUIRE(mxlMonitoringAddFlow(monitoring, id, role, &flow) == MXL_STATUS_OK);
            return flow;
        }

        // A flow at 50 grains a second, so grain n is due at n * 20 ms.
        mxlMonitoringFlow open(mxlMonitoringRole role = MXL_MONITORING_ROLE_READER, char const* id = FlowId)
        {
            auto const flow = add(role, id);
            auto format = mxlMonitoringFlowFormat{};
            format.version = 1;
            format.size = sizeof(format);
            format.format = MXL_DATA_FORMAT_VIDEO;
            format.unit = MXL_MONITORING_UNIT_GRAIN;
            format.rate = mxlRational{50, 1};
            format.mediaType = "video/v210";
            REQUIRE(mxlMonitoringSetFlowFormat(flow, &format) == MXL_STATUS_OK);
            REQUIRE(mxlMonitoringSetFlowTiming(flow, 70 * Ms, 100 * Ms) == MXL_STATUS_OK);
            mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
            return flow;
        }

        mxlMonitoringFlowSnapshot snapshot(std::size_t which = 0) const
        {
            auto all = std::vector<mxlMonitoringFlowSnapshot>(which + 1);
            auto count = all.size();
            REQUIRE(mxlMonitoringSnapshot(monitoring, all.data(), &count) == MXL_STATUS_OK);
            REQUIRE(count > which);
            return all[which];
        }

        // The index of the grain due at the current time.
        std::uint64_t dueIndex() const
        {
            return now / (20 * Ms);
        }

        std::uint64_t now;
        mxlMonitoring monitoring = nullptr;
        std::vector<mxlMonitoringEvent> events;
    };
}
