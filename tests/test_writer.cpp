// The writer wrappers against libmxl itself: a domain on tmpfs and every
// outcome a writer can meet produced for real rather than assumed.

#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>
#include "mxl-monitoring/reader.h"
#include "mxl-monitoring/writer.h"
#include "Support.hpp"

using namespace mxl::monitoring::tests;

namespace
{
    constexpr char const* VideoId = "5fbec3b1-1b0f-417d-9059-8b94a4710011";
    constexpr char const* AudioId = "5fbec3b1-1b0f-417d-9059-8b94a4710012";

    // A writer side on the real clock, in a domain of its own on tmpfs,
    // removed afterwards.
    struct Writer
    {
        explicit Writer(char const* id)
            : path{std::filesystem::path{"/dev/shm"} / ("mxl-monitoring-writer-test-" + std::to_string(::getpid()))}
        {
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
            REQUIRE(mxlMonitoringCreate(nullptr, &monitoring) == MXL_STATUS_OK);
            REQUIRE(mxlMonitoringAddFlow(monitoring, id, MXL_MONITORING_ROLE_WRITER, &flow) == MXL_STATUS_OK);
            instance = mxlMonitoringCreateInstance(flow, path.c_str(), "");
            REQUIRE(instance != nullptr);
        }

        ~Writer()
        {
            if (writer != nullptr)
            {
                mxlReleaseFlowWriter(instance, writer);
            }
            mxlDestroyInstance(instance);
            mxlMonitoringDestroy(monitoring);
            std::filesystem::remove_all(path);
        }

        Writer(Writer const&) = delete;
        Writer& operator=(Writer const&) = delete;

        mxlStatus create(std::string const& definition)
        {
            return mxlMonitoringCreateFlowWriter(flow, instance, definition.c_str(), "", &writer, &config, nullptr);
        }

        mxlMonitoringFlowSnapshot snapshot() const
        {
            auto s = mxlMonitoringFlowSnapshot{};
            auto count = std::size_t{1};
            REQUIRE(mxlMonitoringSnapshot(monitoring, &s, &count) == MXL_STATUS_OK);
            return s;
        }

        // Opens the grain at index and commits validSlices of it, flagged as
        // given.
        mxlStatus writeGrain(std::uint64_t index, std::uint16_t validSlices = MXL_GRAIN_VALID_SLICES_ALL, std::uint32_t flags = 0)
        {
            auto grain = mxlGrainInfo{};
            std::uint8_t* payload = nullptr;
            if (auto const status = mxlMonitoringFlowWriterOpenGrain(flow, writer, index, &grain, &payload); status != MXL_STATUS_OK)
            {
                return status;
            }
            std::memset(payload, 0, grain.grainSize);
            grain.validSlices = validSlices == MXL_GRAIN_VALID_SLICES_ALL ? grain.totalSlices : validSlices;
            grain.flags = flags;
            return mxlMonitoringFlowWriterCommitGrain(flow, writer, &grain);
        }

        std::filesystem::path path;
        mxlMonitoring monitoring = nullptr;
        mxlMonitoringFlow flow = nullptr;
        mxlInstance instance = nullptr;
        mxlFlowWriter writer = nullptr;
        mxlFlowConfigInfo config{};
    };

    std::uint64_t currentIndex()
    {
        auto const rate = mxlRational{50, 1};
        return mxlGetCurrentIndex(&rate);
    }
}

TEST_CASE("A writer that creates its flow has it open, with its format", "[writer]")
{
    auto w = Writer{VideoId};
    REQUIRE(w.create(videoDefinition(VideoId)) == MXL_STATUS_OK);
    auto const s = w.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_OPEN);
    CHECK(s.opens == 1);
    CHECK(s.unit == MXL_MONITORING_UNIT_GRAIN);
    CHECK(s.rate.numerator == 50);
    CHECK(std::string{s.mediaType} == "video/v210");
}

TEST_CASE("Grains count as written once whole, with their commit latency", "[writer]")
{
    auto w = Writer{VideoId};
    REQUIRE(w.create(videoDefinition(VideoId)) == MXL_STATUS_OK);
    auto const index = currentIndex();
    REQUIRE(w.writeGrain(index) == MXL_STATUS_OK);
    REQUIRE(w.writeGrain(index + 1) == MXL_STATUS_OK);
    CHECK(w.snapshot().writes[MXL_MONITORING_WRITE_OK] == 2);
    CHECK(w.snapshot().commitLatency.count == 2);

    INFO("a commit of some slices counts nothing yet");
    auto grain = mxlGrainInfo{};
    std::uint8_t* payload = nullptr;
    REQUIRE(mxlMonitoringFlowWriterOpenGrain(w.flow, w.writer, index + 2, &grain, &payload) == MXL_STATUS_OK);
    grain.validSlices = grain.totalSlices / 2;
    REQUIRE(mxlMonitoringFlowWriterCommitGrain(w.flow, w.writer, &grain) == MXL_STATUS_OK);
    CHECK(w.snapshot().writes[MXL_MONITORING_WRITE_OK] == 2);

    INFO("the commit that completes it counts it");
    grain.validSlices = grain.totalSlices;
    REQUIRE(mxlMonitoringFlowWriterCommitGrain(w.flow, w.writer, &grain) == MXL_STATUS_OK);
    auto const s = w.snapshot();
    CHECK(s.writes[MXL_MONITORING_WRITE_OK] == 3);
    CHECK(s.lastIndex == index + 2);
    CHECK(s.delivering);
}

TEST_CASE("A grain flagged invalid is not written; the writer says why", "[writer]")
{
    auto w = Writer{VideoId};
    REQUIRE(w.create(videoDefinition(VideoId)) == MXL_STATUS_OK);
    auto const index = currentIndex();
    REQUIRE(w.writeGrain(index) == MXL_STATUS_OK);
    REQUIRE(w.writeGrain(index + 1, MXL_GRAIN_VALID_SLICES_ALL, MXL_GRAIN_FLAG_INVALID) == MXL_STATUS_OK);
    CHECK(w.snapshot().writes[MXL_MONITORING_WRITE_OK] == 1);
    mxlMonitoringRecordWriteOutcome(w.flow, MXL_MONITORING_WRITE_INPUT_LATE, MXL_STATUS_OK, index + 1, 1);
    auto const s = w.snapshot();
    CHECK(s.writes[MXL_MONITORING_WRITE_OK] == 1);
    CHECK(s.writes[MXL_MONITORING_WRITE_INPUT_LATE] == 1);
}

TEST_CASE("A grain opened behind the last commit is a write error", "[writer]")
{
    auto w = Writer{VideoId};
    REQUIRE(w.create(videoDefinition(VideoId)) == MXL_STATUS_OK);
    auto const index = currentIndex();
    REQUIRE(w.writeGrain(index) == MXL_STATUS_OK);
    CHECK(w.writeGrain(index) != MXL_STATUS_OK);
    auto const s = w.snapshot();
    CHECK(s.writes[MXL_MONITORING_WRITE_OK] == 1);
    CHECK(s.writes[MXL_MONITORING_WRITE_ERROR] == 1);
    CHECK(s.lastErrorIndex == index);
}

TEST_CASE("Samples committed count in samples", "[writer]")
{
    auto w = Writer{AudioId};
    REQUIRE(w.create(audioDefinition(AudioId)) == MXL_STATUS_OK);
    CHECK(w.snapshot().unit == MXL_MONITORING_UNIT_SAMPLE);
    auto const rate = mxlRational{48000, 1};
    auto const end = mxlGetCurrentIndex(&rate);
    for (auto const index : {end, end + 480})
    {
        auto slices = mxlMutableWrappedMultiBufferSlice{};
        REQUIRE(mxlMonitoringFlowWriterOpenSamples(w.flow, w.writer, index, 480, &slices) == MXL_STATUS_OK);
        REQUIRE(mxlMonitoringFlowWriterCommitSamples(w.flow, w.writer) == MXL_STATUS_OK);
    }
    auto const s = w.snapshot();
    CHECK(s.writes[MXL_MONITORING_WRITE_OK] == 960);
    CHECK(s.lastIndex == end + 480);
}

TEST_CASE("A writer whose domain went away after the instance is domain unavailable", "[writer]")
{
    auto w = Writer{VideoId};
    auto const away = std::filesystem::path{w.path.string() + ".away"};
    std::filesystem::rename(w.path, away);
    auto const status = w.create(videoDefinition(VideoId));
    std::filesystem::rename(away, w.path);
    CHECK(status != MXL_STATUS_OK);
    auto const s = w.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE);
    CHECK(s.accessFailures[MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE] == 1);
    CHECK(s.opens == 0);
    CHECK(s.writes[MXL_MONITORING_WRITE_OUT_OF_SPACE] == 0);
    CHECK(std::string{s.lastErrorDetail} == "MXL domain " + w.path.string() + " does not exist");
}
