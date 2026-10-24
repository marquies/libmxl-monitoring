// What a flow declares, how it differs from what was expected, and which one
// issue a grain or a batch of samples lands in.

#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "mxl-monitoring/content.h"
#include "Support.hpp"

using namespace mxl::monitoring::tests;

namespace
{
    constexpr char const* V210 = R"({"media_type": "video/v210", "frame_width": 1296, "frame_height": 720,
        "interlace_mode": "progressive", "grain_rate": {"numerator": 30000, "denominator": 1001}})";

    mxlMonitoringDeclared v210()
    {
        auto d = mxlMonitoringDeclared{};
        REQUIRE(mxlMonitoringParseDefinition(V210, &d) == MXL_STATUS_OK);
        return d;
    }

    std::uint64_t rowBytes(int width)
    {
        return static_cast<std::uint64_t>((width + 47) / 48) * 128;
    }

    mxlGrainInfo wholeGrain(std::uint64_t index, mxlMonitoringDeclared const& d)
    {
        auto grain = mxlGrainInfo{};
        grain.index = index;
        grain.totalSlices = static_cast<std::uint16_t>(d.height);
        grain.validSlices = grain.totalSlices;
        grain.grainSize = static_cast<std::uint32_t>(rowBytes(d.width) * static_cast<std::uint64_t>(d.height));
        return grain;
    }

    // A v210 picture of mid grey: every sample code 512, legal.
    std::vector<std::uint8_t> greyPicture(mxlMonitoringDeclared const& d)
    {
        auto bytes = std::vector<std::uint8_t>(rowBytes(d.width) * static_cast<std::uint64_t>(d.height));
        auto const word = std::uint32_t{512U | (512U << 10) | (512U << 20)};
        for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4)
        {
            std::memcpy(bytes.data() + i, &word, 4);
        }
        return bytes;
    }

    mxlMonitoringExpectation expectation()
    {
        auto e = mxlMonitoringExpectation{};
        e.version = 1;
        e.size = sizeof(e);
        return e;
    }

    struct Check
    {
        bool found;
        mxlMonitoringContent issue;
        std::string detail;
    };

    Check checkGrain(mxlMonitoringDeclared const& d, std::uint32_t mismatch, bool checkLegal, mxlGrainInfo const& grain, std::uint64_t index,
        std::vector<std::uint8_t> const& picture)
    {
        auto c = Check{};
        char detail[MXL_MONITORING_TEXT];
        c.found = mxlMonitoringCheckGrain(&d, mismatch, checkLegal, &grain, index, picture.data(), &c.issue, detail, sizeof(detail));
        c.detail = detail;
        return c;
    }
}

TEST_CASE("A flow definition is read for what the checks need", "[content]")
{
    auto const d = v210();
    CHECK(std::string{d.mediaType} == "video/v210");
    CHECK(d.width == 1296);
    CHECK(d.height == 720);
    CHECK(d.rate.numerator == 30000);
    CHECK(d.rate.denominator == 1001);
    CHECK_FALSE(d.interlaced);

    auto field = mxlMonitoringDeclared{};
    REQUIRE(mxlMonitoringParseDefinition(R"({"media_type": "video/v210", "frame_width": 1920, "frame_height": 1080,
        "interlace_mode": "interlaced_tff"})",
                &field) == MXL_STATUS_OK);
    CHECK(field.interlaced);

    auto audio = mxlMonitoringDeclared{};
    REQUIRE(mxlMonitoringParseDefinition(R"({"media_type": "audio/float32", "sample_rate": {"numerator": 48000}, "channel_count": 2})", &audio) ==
            MXL_STATUS_OK);
    CHECK(audio.sampleRate == 48000);
    CHECK(audio.channels == 2);

    auto none = mxlMonitoringDeclared{};
    CHECK(mxlMonitoringParseDefinition("not json", &none) == MXL_ERR_INVALID_ARG);
}

TEST_CASE("A mismatch names each way the flow differs, and which issue it is", "[content]")
{
    auto const d = v210();
    auto e = expectation();
    char message[MXL_MONITORING_TEXT];
    CHECK(mxlMonitoringMismatch(&d, &e, message, sizeof(message)) == 0);
    CHECK(std::string{message}.empty());

    e.width = 1296;
    e.height = 720;
    e.rate = mxlRational{60000, 2002};
    CHECK(mxlMonitoringMismatch(&d, &e, message, sizeof(message)) == 0);

    e.width = 1920;
    e.height = 1080;
    CHECK(mxlMonitoringMismatch(&d, &e, message, sizeof(message)) == (1U << MXL_MONITORING_CONTENT_WRONG_RESOLUTION));
    CHECK(std::string{message} == "1296x720 where 1920x1080 was expected");

    e.mediaType = "video/v216";
    e.rate = mxlRational{50, 1};
    auto const bits = mxlMonitoringMismatch(&d, &e, message, sizeof(message));
    CHECK(bits == ((1U << MXL_MONITORING_CONTENT_WRONG_FORMAT) | (1U << MXL_MONITORING_CONTENT_WRONG_RESOLUTION) |
                      (1U << MXL_MONITORING_CONTENT_WRONG_RATE)));
    CHECK(std::string{message} ==
          "media type video/v210 where video/v216 was expected; 1296x720 where 1920x1080 was expected; rate 30000/1001 where 50/1 was expected");
}

TEST_CASE("A grain lands in exactly one issue, worst first", "[content]")
{
    auto const d = v210();
    auto picture = greyPicture(d);
    auto const grain = wholeGrain(7, d);

    CHECK_FALSE(checkGrain(d, 0, true, grain, 7, picture).found);

    auto c = checkGrain(d, 0, false, grain, 8, picture);
    CHECK(c.found);
    CHECK(c.issue == MXL_MONITORING_CONTENT_MISSING);

    auto invalid = grain;
    invalid.flags = MXL_GRAIN_FLAG_INVALID;
    invalid.validSlices = 0;
    c = checkGrain(d, 0, false, invalid, 7, picture);
    INFO("a gap-fill grain counts once, as invalid, not also as incomplete");
    CHECK(c.issue == MXL_MONITORING_CONTENT_INVALID);

    auto partial = grain;
    partial.validSlices = 100;
    CHECK(checkGrain(d, 0, false, partial, 7, picture).issue == MXL_MONITORING_CONTENT_INCOMPLETE);

    auto small = grain;
    small.grainSize -= 128;
    CHECK(checkGrain(d, 0, false, small, 7, picture).issue == MXL_MONITORING_CONTENT_WRONG_SIZE);

    INFO("a flow that differs from its expectation counts as that, whatever else is wrong");
    CHECK(checkGrain(d, 1U << MXL_MONITORING_CONTENT_WRONG_RATE, false, small, 8, picture).issue == MXL_MONITORING_CONTENT_WRONG_RATE);
    CHECK(checkGrain(d, (1U << MXL_MONITORING_CONTENT_WRONG_RATE) | (1U << MXL_MONITORING_CONTENT_WRONG_FORMAT), false, small, 8, picture).issue ==
          MXL_MONITORING_CONTENT_WRONG_FORMAT);
}

TEST_CASE("Sample values are checked only when asked, and located", "[content]")
{
    auto const d = v210();
    auto picture = greyPicture(d);
    auto const grain = wholeGrain(7, d);
    auto const zero = std::uint32_t{0};
    std::memcpy(picture.data() + rowBytes(d.width) * 719 + 8, &zero, 4);

    CHECK_FALSE(checkGrain(d, 0, false, grain, 7, picture).found);
    auto const c = checkGrain(d, 0, true, grain, 7, picture);
    CHECK(c.issue == MXL_MONITORING_CONTENT_ILLEGAL_VALUES);
    CHECK(c.detail == "v210 code 0 at row 719, word 2, bit 0");
}

TEST_CASE("Float audio is legal within -1..1 and when it is a number", "[content]")
{
    auto d = mxlMonitoringDeclared{};
    REQUIRE(mxlMonitoringParseDefinition(R"({"media_type": "audio/float32"})", &d) == MXL_STATUS_OK);
    auto left = std::vector<float>{0.5F, -0.25F, 1.0F};
    auto slices = mxlWrappedMultiBufferSlice{};
    slices.base.fragments[0].pointer = left.data();
    slices.base.fragments[0].size = left.size() * sizeof(float);
    slices.count = 1;
    auto issue = mxlMonitoringContent{};

    CHECK_FALSE(mxlMonitoringCheckSamples(&d, 0, true, &slices, &issue, nullptr, 0));
    left[1] = 1.5F;
    CHECK_FALSE(mxlMonitoringCheckSamples(&d, 0, false, &slices, &issue, nullptr, 0));
    CHECK(mxlMonitoringCheckSamples(&d, 0, true, &slices, &issue, nullptr, 0));
    CHECK(issue == MXL_MONITORING_CONTENT_ILLEGAL_VALUES);
    left[1] = std::nanf("");
    CHECK(mxlMonitoringCheckSamples(&d, 0, true, &slices, &issue, nullptr, 0));
}

TEST_CASE("A flow keeps its definition and expectation and reports the mismatch", "[content]")
{
    auto context = Context{};
    auto const flow = context.add();
    auto e = expectation();
    e.width = 1920;
    e.height = 1080;
    e.checkLegal = true;
    REQUIRE(mxlMonitoringSetExpectation(flow, &e) == MXL_STATUS_OK);
    CHECK(std::string{context.snapshot().formatMismatch}.empty());

    REQUIRE(mxlMonitoringSetDefinition(flow, V210) == MXL_STATUS_OK);
    CHECK(std::string{context.snapshot().formatMismatch} == "1296x720 where 1920x1080 was expected");

    auto declared = mxlMonitoringDeclared{};
    REQUIRE(mxlMonitoringGetDeclared(flow, &declared) == MXL_STATUS_OK);
    CHECK(declared.width == 1296);
}
