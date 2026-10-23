// Recording threads, a polling thread and a snapshot thread at once, as in a
// process with one read loop per flow and an exporter. Counts must add up
// exactly; built with -fsanitize=thread, the same run finds any data race.

#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "mxl-monitoring/prometheus.h"
#include "Support.hpp"

using namespace mxl::monitoring::tests;

TEST_CASE("Concurrent recording, polling and snapshots lose no count", "[concurrency]")
{
    constexpr int Flows = 4;
    constexpr int Reads = 20'000;

    auto monitoring = mxlMonitoring{};
    REQUIRE(mxlMonitoringCreate(nullptr, &monitoring) == MXL_STATUS_OK);
    auto events = std::atomic<std::uint64_t>{0};
    REQUIRE(mxlMonitoringSubscribe(monitoring, [](mxlMonitoringEvent const*, void* user) { ++*static_cast<std::atomic<std::uint64_t>*>(user); },
                &events) == MXL_STATUS_OK);

    auto flows = std::vector<mxlMonitoringFlow>(Flows);
    for (int i = 0; i < Flows; ++i)
    {
        auto const id = "d4d00000-0000-0000-0000-00000000000" + std::to_string(i);
        REQUIRE(mxlMonitoringAddFlow(monitoring, id.c_str(), MXL_MONITORING_ROLE_READER, &flows[i]) == MXL_STATUS_OK);
        auto format = mxlMonitoringFlowFormat{};
        format.version = 1;
        format.size = sizeof(format);
        format.unit = MXL_MONITORING_UNIT_GRAIN;
        format.rate = mxlRational{50, 1};
        format.mediaType = "video/v210";
        REQUIRE(mxlMonitoringSetFlowFormat(flows[i], &format) == MXL_STATUS_OK);
    }

    auto done = std::atomic<bool>{false};
    auto readers = std::vector<std::thread>{};
    for (auto const flow : flows)
    {
        readers.emplace_back(
            [flow]
            {
                mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
                for (std::uint64_t i = 0; i < Reads; ++i)
                {
                    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, i, 1);
                    if (i % 100 == 0)
                    {
                        mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, i, 1);
                        mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_INCOMPLETE, i, 1);
                    }
                }
            });
    }
    auto poller = std::thread{[&]
        {
            while (!done)
            {
                mxlMonitoringPoll(monitoring);
            }
        }};
    auto exporter = std::thread{[&]
        {
            auto size = std::size_t{0};
            auto buffer = std::vector<char>(1 << 20);
            while (!done)
            {
                size = buffer.size();
                mxlMonitoringPrometheusRender(monitoring, buffer.data(), &size);
            }
        }};

    for (auto& t : readers)
    {
        t.join();
    }
    done = true;
    poller.join();
    exporter.join();
    mxlMonitoringPoll(monitoring);

    auto snapshots = std::vector<mxlMonitoringFlowSnapshot>(Flows);
    auto count = snapshots.size();
    REQUIRE(mxlMonitoringSnapshot(monitoring, snapshots.data(), &count) == MXL_STATUS_OK);
    for (auto const& s : snapshots)
    {
        CHECK(s.reads[MXL_MONITORING_READ_OK] == Reads);
        CHECK(s.reads[MXL_MONITORING_READ_TOO_LATE] == Reads / 100);
        CHECK(s.content[MXL_MONITORING_CONTENT_INCOMPLETE] == Reads / 100);
        CHECK(s.arrivalLatency.count == Reads + Reads / 100);
    }
    INFO("each flow started both conditions at least once");
    CHECK(events >= 2 * Flows);
    mxlMonitoringDestroy(monitoring);
}

TEST_CASE("Flows come and go while polls and snapshots run", "[concurrency]")
{
    auto monitoring = mxlMonitoring{};
    REQUIRE(mxlMonitoringCreate(nullptr, &monitoring) == MXL_STATUS_OK);
    REQUIRE(mxlMonitoringSubscribe(monitoring, [](mxlMonitoringEvent const*, void*) {}, nullptr) == MXL_STATUS_OK);
    auto done = std::atomic<bool>{false};
    auto churn = std::thread{[&]
        {
            for (int i = 0; i < 2'000; ++i)
            {
                auto flow = mxlMonitoringFlow{};
                REQUIRE(mxlMonitoringAddFlow(monitoring, "d4d00000-0000-0000-0000-000000000009", MXL_MONITORING_ROLE_READER, &flow) ==
                        MXL_STATUS_OK);
                mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_FLOW_NOT_FOUND, MXL_ERR_FLOW_NOT_FOUND);
                mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 1, 1);
                REQUIRE(mxlMonitoringRemoveFlow(monitoring, flow) == MXL_STATUS_OK);
            }
            done = true;
        }};
    auto poller = std::thread{[&]
        {
            while (!done)
            {
                mxlMonitoringPoll(monitoring);
            }
        }};
    auto buffer = std::vector<char>(1 << 16);
    while (!done)
    {
        auto size = buffer.size();
        mxlMonitoringPrometheusRender(monitoring, buffer.data(), &size);
    }
    churn.join();
    poller.join();
    auto count = std::size_t{0};
    REQUIRE(mxlMonitoringSnapshot(monitoring, nullptr, &count) == MXL_STATUS_OK);
    CHECK(count == 0);
    mxlMonitoringDestroy(monitoring);
}
