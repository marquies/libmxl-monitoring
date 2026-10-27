// The reader wrappers against libmxl itself: a domain on tmpfs, a flow written
// with libmxl's own writer, and every outcome a reader can meet produced for
// real rather than assumed.

#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>
#include "mxl-monitoring/content.h"
#include "mxl-monitoring/reader.h"
#include "Support.hpp"

using namespace mxl::monitoring::tests;

namespace
{
    constexpr char const* VideoId = "5fbec3b1-1b0f-417d-9059-8b94a4710001";
    constexpr char const* AudioId = "5fbec3b1-1b0f-417d-9059-8b94a4710002";

    std::string videoDefinition(char const* id)
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

    std::string audioDefinition(char const* id)
    {
        return std::string{R"({"id": ")"} + id + R"(", "description": "test", "label": "test",
            "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["libmxl-monitoring-tests:Audio"]},
            "format": "urn:x-nmos:format:audio", "parents": [], "media_type": "audio/float32",
            "sample_rate": {"numerator": 48000}, "channel_count": 2, "bit_depth": 32})";
    }

    // A domain of its own per test, on tmpfs, removed afterwards.
    struct Domain
    {
        Domain()
            : path{std::filesystem::path{"/dev/shm"} / ("mxl-monitoring-test-" + std::to_string(::getpid()))}
        {
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
            writerInstance = mxlCreateInstance(path.c_str(), "");
            REQUIRE(writerInstance != nullptr);
        }

        ~Domain()
        {
            if (writer != nullptr)
            {
                mxlReleaseFlowWriter(writerInstance, writer);
            }
            mxlDestroyInstance(writerInstance);
            std::filesystem::remove_all(path);
        }

        Domain(Domain const&) = delete;
        Domain& operator=(Domain const&) = delete;

        void createWriter(std::string const& definition)
        {
            auto created = false;
            REQUIRE(mxlCreateFlowWriter(writerInstance, definition.c_str(), "", &writer, nullptr, &created) == MXL_STATUS_OK);
        }

        // Commits a grain of mid grey at index, whole unless told how many
        // slices are valid.
        void writeGrain(std::uint64_t index, std::uint16_t validSlices = MXL_GRAIN_VALID_SLICES_ALL)
        {
            auto info = mxlGrainInfo{};
            std::uint8_t* payload = nullptr;
            REQUIRE(mxlFlowWriterOpenGrain(writer, index, &info, &payload) == MXL_STATUS_OK);
            auto const word = std::uint32_t{512U | (512U << 10) | (512U << 20)};
            for (std::size_t i = 0; i + 4 <= info.grainSize; i += 4)
            {
                std::memcpy(payload + i, &word, 4);
            }
            info.validSlices = validSlices == MXL_GRAIN_VALID_SLICES_ALL ? info.totalSlices : validSlices;
            REQUIRE(mxlFlowWriterCommitGrain(writer, &info) == MXL_STATUS_OK);
        }

        void writeSamples(std::uint64_t index, std::size_t count)
        {
            auto slices = mxlMutableWrappedMultiBufferSlice{};
            REQUIRE(mxlFlowWriterOpenSamples(writer, index, count, &slices) == MXL_STATUS_OK);
            for (std::size_t channel = 0; channel < slices.count; ++channel)
            {
                for (auto const& fragment : slices.base.fragments)
                {
                    if (fragment.pointer != nullptr)
                    {
                        std::memset(static_cast<std::uint8_t*>(fragment.pointer) + channel * slices.stride, 0, fragment.size);
                    }
                }
            }
            REQUIRE(mxlFlowWriterCommitSamples(writer) == MXL_STATUS_OK);
        }

        std::filesystem::path path;
        mxlInstance writerInstance = nullptr;
        mxlFlowWriter writer = nullptr;
    };

    // A reader side on the real clock, with the libmxl handles released.
    struct Reader
    {
        Reader(Domain const& domain, char const* id)
        {
            REQUIRE(mxlMonitoringCreate(nullptr, &monitoring) == MXL_STATUS_OK);
            REQUIRE(mxlMonitoringAddFlow(monitoring, id, MXL_MONITORING_ROLE_READER, &flow) == MXL_STATUS_OK);
            instance = mxlMonitoringCreateInstance(flow, domain.path.c_str(), "");
            REQUIRE(instance != nullptr);
        }

        ~Reader()
        {
            if (reader != nullptr)
            {
                mxlReleaseFlowReader(instance, reader);
            }
            mxlDestroyInstance(instance);
            mxlMonitoringDestroy(monitoring);
        }

        Reader(Reader const&) = delete;
        Reader& operator=(Reader const&) = delete;

        mxlMonitoringFlowSnapshot snapshot() const
        {
            auto s = mxlMonitoringFlowSnapshot{};
            auto count = std::size_t{1};
            REQUIRE(mxlMonitoringSnapshot(monitoring, &s, &count) == MXL_STATUS_OK);
            return s;
        }

        mxlStatus readGrain(std::uint64_t index, std::uint16_t minValidSlices = MXL_GRAIN_VALID_SLICES_ALL)
        {
            auto grain = mxlGrainInfo{};
            std::uint8_t* payload = nullptr;
            return mxlMonitoringFlowReaderGetGrainSliceNonBlocking(flow, reader, index, minValidSlices, &grain, &payload);
        }

        mxlMonitoring monitoring = nullptr;
        mxlMonitoringFlow flow = nullptr;
        mxlInstance instance = nullptr;
        mxlFlowReader reader = nullptr;
    };

    std::uint64_t currentIndex()
    {
        auto const rate = mxlRational{50, 1};
        return mxlGetCurrentIndex(&rate);
    }
}

TEST_CASE("A domain that does not exist is unavailable", "[reader]")
{
    auto monitoring = mxlMonitoring{};
    REQUIRE(mxlMonitoringCreate(nullptr, &monitoring) == MXL_STATUS_OK);
    auto flow = mxlMonitoringFlow{};
    REQUIRE(mxlMonitoringAddFlow(monitoring, VideoId, MXL_MONITORING_ROLE_READER, &flow) == MXL_STATUS_OK);
    auto const instance = mxlMonitoringCreateInstance(flow, "/dev/shm/mxl-monitoring-test-absent/domain", "");
    CHECK(instance == nullptr);
    auto s = mxlMonitoringFlowSnapshot{};
    auto count = std::size_t{1};
    REQUIRE(mxlMonitoringSnapshot(monitoring, &s, &count) == MXL_STATUS_OK);
    CHECK(s.access == MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE);
    CHECK(s.accessFailures[MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE] == 1);
    CHECK(std::string{s.lastErrorDetail} == "MXL domain /dev/shm/mxl-monitoring-test-absent/domain does not exist");
    if (instance != nullptr)
    {
        mxlDestroyInstance(instance);
    }
    mxlMonitoringDestroy(monitoring);
}

TEST_CASE("A flow not in the domain is not found", "[reader]")
{
    auto domain = Domain{};
    auto reader = Reader{domain, VideoId};
    CHECK(mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader) != MXL_STATUS_OK);
    auto const s = reader.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_FLOW_NOT_FOUND);
    CHECK(s.accessFailures[MXL_MONITORING_ACCESS_FLOW_NOT_FOUND] == 1);
    CHECK(std::string{s.lastErrorDetail} == std::string{"flow "} + VideoId + " not found in the domain");
}

TEST_CASE("A flow whose domain went away after the instance is domain unavailable", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(videoDefinition(VideoId));
    auto reader = Reader{domain, VideoId};
    auto const away = std::filesystem::path{domain.path.string() + ".away"};
    std::filesystem::rename(domain.path, away);
    INFO("libmxl answers as for a flow not in the domain");
    auto const status = mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader);
    std::filesystem::rename(away, domain.path);
    CHECK(status == MXL_ERR_FLOW_NOT_FOUND);
    auto const s = reader.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE);
    CHECK(s.accessFailures[MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE] == 1);
    CHECK(s.accessFailures[MXL_MONITORING_ACCESS_FLOW_NOT_FOUND] == 0);
    CHECK(std::string{s.lastErrorDetail} == "MXL domain " + domain.path.string() + " does not exist");
}

TEST_CASE("Opening a flow reads what it is", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(videoDefinition(VideoId));
    auto reader = Reader{domain, VideoId};
    REQUIRE(mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader) == MXL_STATUS_OK);
    auto const s = reader.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_OPEN);
    CHECK(s.opens == 1);
    CHECK(s.unit == MXL_MONITORING_UNIT_GRAIN);
    CHECK(s.format == MXL_DATA_FORMAT_VIDEO);
    CHECK(s.rate.numerator == 50);
    CHECK(s.rate.denominator == 1);
    CHECK(std::string{s.mediaType} == "video/v210");
}

TEST_CASE("Grains read at the head count as OK, with their commit time", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(videoDefinition(VideoId));
    auto reader = Reader{domain, VideoId};
    REQUIRE(mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader) == MXL_STATUS_OK);
    auto const index = currentIndex();
    domain.writeGrain(index);
    REQUIRE(reader.readGrain(index) == MXL_STATUS_OK);
    auto const s = reader.snapshot();
    CHECK(s.reads[MXL_MONITORING_READ_OK] == 1);
    CHECK(s.reads[MXL_MONITORING_READ_INVALID_CONTENT] == 0);
    CHECK(s.lastIndex == index);
    CHECK(s.delivering);
    CHECK(s.arrivalLatency.count == 1);
    CHECK(s.commitLatency.count == 1);
}

TEST_CASE("A slot taken as it stands, never written for its index, is missing", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(videoDefinition(VideoId));
    auto reader = Reader{domain, VideoId};
    REQUIRE(mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader) == MXL_STATUS_OK);
    auto const index = currentIndex();
    // The writer's first grain: libmxl invalidates a gap only behind an
    // earlier commit, so the slot before it stays as it was.
    domain.writeGrain(index + 2);
    INFO("whole grains only: the gap is not there yet");
    CHECK(reader.readGrain(index + 1) == MXL_ERR_OUT_OF_RANGE_TOO_EARLY);
    INFO("taken as it stands, the slot still holds an older grain");
    REQUIRE(reader.readGrain(index + 1, MXL_GRAIN_VALID_SLICES_ANY) == MXL_STATUS_OK);
    auto const s = reader.snapshot();
    CHECK(s.content[MXL_MONITORING_CONTENT_MISSING] == 1);
    CHECK(s.reads[MXL_MONITORING_READ_OK] == 0);
}

TEST_CASE("A grain a writer jumped over is invalid", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(videoDefinition(VideoId));
    auto reader = Reader{domain, VideoId};
    REQUIRE(mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader) == MXL_STATUS_OK);
    auto const index = currentIndex();
    domain.writeGrain(index);
    domain.writeGrain(index + 2);
    INFO("libmxl marks the grain between them invalid as the writer opens index + 2");
    REQUIRE(reader.readGrain(index + 1) == MXL_STATUS_OK);
    auto const s = reader.snapshot();
    CHECK(s.content[MXL_MONITORING_CONTENT_INVALID] == 1);
    CHECK(s.content[MXL_MONITORING_CONTENT_MISSING] == 0);
    CHECK(s.reads[MXL_MONITORING_READ_OK] == 0);
}

TEST_CASE("A grain left part-written is incomplete", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(videoDefinition(VideoId));
    auto reader = Reader{domain, VideoId};
    REQUIRE(mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader) == MXL_STATUS_OK);
    auto const index = currentIndex();
    domain.writeGrain(index, 10);
    REQUIRE(reader.readGrain(index, MXL_GRAIN_VALID_SLICES_ANY) == MXL_STATUS_OK);
    CHECK(reader.snapshot().content[MXL_MONITORING_CONTENT_INCOMPLETE] == 1);
}

TEST_CASE("A grain the ring has overwritten is too late", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(videoDefinition(VideoId));
    auto reader = Reader{domain, VideoId};
    REQUIRE(mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader) == MXL_STATUS_OK);
    auto const index = currentIndex();
    domain.writeGrain(index);
    CHECK(reader.readGrain(index - 1000) == MXL_ERR_OUT_OF_RANGE_TOO_LATE);
    auto const s = reader.snapshot();
    CHECK(s.reads[MXL_MONITORING_READ_TOO_LATE] == 1);
    CHECK(s.lastErrorIndex == index - 1000);
}

TEST_CASE("A flow other than expected counts every grain as wrong", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(videoDefinition(VideoId));
    auto reader = Reader{domain, VideoId};
    auto expected = mxlMonitoringExpectation{};
    expected.version = 1;
    expected.size = sizeof(expected);
    expected.width = 1920;
    expected.height = 1080;
    REQUIRE(mxlMonitoringSetExpectation(reader.flow, &expected) == MXL_STATUS_OK);
    REQUIRE(mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader) == MXL_STATUS_OK);
    auto const index = currentIndex();
    domain.writeGrain(index);
    REQUIRE(reader.readGrain(index) == MXL_STATUS_OK);
    auto const s = reader.snapshot();
    CHECK(s.content[MXL_MONITORING_CONTENT_WRONG_RESOLUTION] == 1);
    CHECK(s.reads[MXL_MONITORING_READ_OK] == 0);
    CHECK(std::string{s.formatMismatch} == "96x54 where 1920x1080 was expected");
    CHECK(std::string{s.lastErrorDetail} == "format differs from what was expected: 96x54 where 1920x1080 was expected");

    auto events = std::vector<mxlMonitoringEvent>{};
    REQUIRE(mxlMonitoringSubscribe(reader.monitoring,
                [](mxlMonitoringEvent const* event, void* user) { static_cast<std::vector<mxlMonitoringEvent>*>(user)->push_back(*event); },
                &events) == MXL_STATUS_OK);
    CHECK(reader.readGrain(index - 1000) == MXL_ERR_OUT_OF_RANGE_TOO_LATE);
    mxlMonitoringPoll(reader.monitoring);
    REQUIRE(events.size() == 2);
    for (auto const& event : events)
    {
        INFO("only the mismatch's own event carries its text");
        if (event.read == MXL_MONITORING_READ_TOO_LATE)
        {
            CHECK(std::string{event.detail}.empty());
        }
        else
        {
            CHECK(event.content == MXL_MONITORING_CONTENT_WRONG_RESOLUTION);
            CHECK(std::string{event.detail} == "format differs from what was expected: 96x54 where 1920x1080 was expected");
        }
    }
}

TEST_CASE("Samples read count in samples", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(audioDefinition(AudioId));
    auto reader = Reader{domain, AudioId};
    REQUIRE(mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader) == MXL_STATUS_OK);
    auto const rate = mxlRational{48000, 1};
    auto const head = mxlGetCurrentIndex(&rate);
    domain.writeSamples(head, 480);
    auto slices = mxlWrappedMultiBufferSlice{};
    REQUIRE(mxlMonitoringFlowReaderGetSamplesNonBlocking(reader.flow, reader.reader, head, 480, &slices) == MXL_STATUS_OK);
    auto const s = reader.snapshot();
    CHECK(s.unit == MXL_MONITORING_UNIT_SAMPLE);
    CHECK(s.reads[MXL_MONITORING_READ_OK] == 480);
}

TEST_CASE("A slot reported under its own name opens the flow it is pointed at", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(videoDefinition(VideoId));
    auto monitoring = mxlMonitoring{};
    REQUIRE(mxlMonitoringCreate(nullptr, &monitoring) == MXL_STATUS_OK);
    auto flow = mxlMonitoringFlow{};
    REQUIRE(mxlMonitoringAddFlow(monitoring, "camera-1", MXL_MONITORING_ROLE_READER, &flow) == MXL_STATUS_OK);
    auto const instance = mxlMonitoringCreateInstance(flow, domain.path.c_str(), "");
    REQUIRE(instance != nullptr);
    auto reader = mxlFlowReader{};

    INFO("pointed at nothing, there is nothing to open");
    REQUIRE(mxlMonitoringSetFlowTarget(flow, "") == MXL_STATUS_OK);
    CHECK(mxlMonitoringGetFlowAccess(flow) == MXL_MONITORING_ACCESS_DISCONNECTED);
    CHECK(mxlMonitoringCreateFlowReader(flow, instance, "", &reader) == MXL_ERR_INVALID_ARG);

    REQUIRE(mxlMonitoringSetFlowTarget(flow, VideoId) == MXL_STATUS_OK);
    REQUIRE(mxlMonitoringCreateFlowReader(flow, instance, "", &reader) == MXL_STATUS_OK);
    auto s = mxlMonitoringFlowSnapshot{};
    auto count = std::size_t{1};
    REQUIRE(mxlMonitoringSnapshot(monitoring, &s, &count) == MXL_STATUS_OK);
    CHECK(std::string{s.flowId} == "camera-1");
    CHECK(std::string{s.target} == VideoId);
    CHECK(s.access == MXL_MONITORING_ACCESS_OPEN);
    CHECK(std::string{s.mediaType} == "video/v210");

    mxlReleaseFlowReader(instance, reader);
    mxlDestroyInstance(instance);
    mxlMonitoringDestroy(monitoring);
}

TEST_CASE("A flow whose definition cannot be read is not open", "[reader]")
{
    auto domain = Domain{};
    domain.createWriter(videoDefinition(VideoId));
    auto const def = domain.path / (std::string{VideoId} + ".mxl-flow") / "flow_def.json";
    REQUIRE(std::filesystem::exists(def));
    std::filesystem::resize_file(def, 10);
    auto reader = Reader{domain, VideoId};
    CHECK(mxlMonitoringCreateFlowReader(reader.flow, reader.instance, "", &reader.reader) != MXL_STATUS_OK);
    CHECK(reader.reader == nullptr);
    auto const s = reader.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_OTHER);
    CHECK(s.accessFailures[MXL_MONITORING_ACCESS_OTHER] == 1);
    CHECK(s.opens == 0);
    CHECK(std::string{s.lastErrorDetail} == std::string{"cannot read the definition of flow "} + VideoId);
}
