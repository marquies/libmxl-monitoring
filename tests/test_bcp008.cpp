// What a BCP-008 monitor is handed for a flow. A Node polls a snapshot per
// flow, so every status is judged on what happened between two snapshots:
// a fault counted before the previous one has already been reported, and
// reporting it again would hold the monitor unhealthy forever.

#include <cstdio>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "mxl-monitoring/bcp008.h"
#include "Support.hpp"

using namespace mxl::monitoring::tests;

namespace
{
    mxlMonitoringBcp008Statuses statuses(mxlMonitoringFlowSnapshot const* previous, mxlMonitoringFlowSnapshot const& current)
    {
        auto out = mxlMonitoringBcp008Statuses{};
        out.version = 1;
        out.size = sizeof(out);
        REQUIRE(mxlMonitoringBcp008GetStatuses(previous, &current, &out) == MXL_STATUS_OK);
        return out;
    }

    struct Named
    {
        std::string name;
        std::uint64_t value;

        bool operator==(Named const&) const = default;
    };

    std::vector<Named> counters(mxlMonitoringFlowSnapshot const* baseline, mxlMonitoringFlowSnapshot const& current,
        mxlMonitoringBcp008Counters which)
    {
        auto count = std::size_t{0};
        REQUIRE(mxlMonitoringBcp008GetCounters(baseline, &current, which, nullptr, &count) == MXL_STATUS_OK);
        auto filled = std::vector<mxlMonitoringBcp008Counter>(count);
        REQUIRE(mxlMonitoringBcp008GetCounters(baseline, &current, which, filled.data(), &count) == MXL_STATUS_OK);
        auto out = std::vector<Named>{};
        for (auto const& counter : filled)
        {
            CHECK(counter.description != nullptr);
            out.push_back({counter.name, counter.value});
        }
        return out;
    }

    // A reader or writer at the head, 50 grains a second.
    void keepUp(Context& context, mxlMonitoringFlow flow, int grains)
    {
        for (int i = 0; i < grains; ++i)
        {
            context.now += 20 * Ms;
            if (context.snapshot().role == MXL_MONITORING_ROLE_WRITER)
            {
                mxlMonitoringRecordWrite(flow, MXL_STATUS_OK, context.dueIndex(), 1);
            }
            else
            {
                mxlMonitoringRecordRead(flow, MXL_STATUS_OK, context.dueIndex(), 1);
            }
        }
    }
}

TEST_CASE("A receiver reading at the head is healthy, and never uses external synchronization", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open();
    keepUp(context, flow, 3);
    auto const before = context.snapshot();
    keepUp(context, flow, 3);
    auto const s = statuses(&before, context.snapshot());
    CHECK(s.link.status == MXL_MONITORING_BCP008_HEALTHY);
    CHECK(s.connection.status == MXL_MONITORING_BCP008_HEALTHY);
    CHECK(s.stream.status == MXL_MONITORING_BCP008_HEALTHY);
    CHECK(s.synchronization.status == MXL_MONITORING_BCP008_INACTIVE); // NotUsed
    CHECK(std::string{s.connection.message}.empty());
}

TEST_CASE("A disconnected receiver is inactive, with its link up", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open();
    keepUp(context, flow, 3);
    auto const before = context.snapshot();
    REQUIRE(mxlMonitoringSetFlowTarget(flow, "") == MXL_STATUS_OK);
    auto const s = statuses(&before, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_INACTIVE);
    CHECK(s.stream.status == MXL_MONITORING_BCP008_INACTIVE);
    CHECK(s.link.status == MXL_MONITORING_BCP008_HEALTHY);
}

TEST_CASE("An unavailable domain is the link all down and the connection unhealthy", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.add();
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE, MXL_ERR_UNKNOWN);
    auto const s = statuses(nullptr, context.snapshot());
    CHECK(s.link.status == MXL_MONITORING_BCP008_UNHEALTHY); // AllDown
    CHECK(s.link.message[0] != '\0');
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.connection.message}.find("domain_unavailable") != std::string::npos);
    INFO("nothing was read, so nothing says whether the stream decodes");
    CHECK(s.stream.status == MXL_MONITORING_BCP008_UNCHANGED);
}

TEST_CASE("A receiver that reads nothing is unhealthy, even with the flow open", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open();
    keepUp(context, flow, 3);
    auto const before = context.snapshot();
    context.now += 2 * Second;
    auto const s = statuses(&before, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.connection.message} == "nothing read within the last second");
    CHECK(s.stream.status == MXL_MONITORING_BCP008_UNCHANGED);
}

TEST_CASE("Grains lost or late since the previous snapshot make the connection unhealthy, once", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open();
    keepUp(context, flow, 3);
    auto const before = context.snapshot();
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, context.dueIndex() - 10, 1);
    mxlMonitoringRecordSkipped(flow, 4);
    keepUp(context, flow, 1);
    auto const lost = context.snapshot();
    auto s = statuses(&before, lost);
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.connection.message} == "4 grains lost");

    keepUp(context, flow, 3);
    CHECK(statuses(&lost, context.snapshot()).connection.status == MXL_MONITORING_BCP008_HEALTHY);

    auto const healthy = context.snapshot();
    auto const index = context.dueIndex() + 1;
    context.now += 95 * Ms; // due 20 ms in, grace 70 ms, stall at 100 ms
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, index, 1);
    s = statuses(&healthy, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.connection.message} == "1 late read");
}

TEST_CASE("The stream status is the worst content issue read since the previous snapshot", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open();
    keepUp(context, flow, 3);
    auto const before = context.snapshot();

    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_INCOMPLETE, context.dueIndex(), 1);
    auto s = statuses(&before, context.snapshot());
    CHECK(s.stream.status == MXL_MONITORING_BCP008_PARTIALLY_HEALTHY);
    CHECK(std::string{s.stream.message} == "1 grain incomplete");

    INFO("invalid comes after wrong resolution in the enum, and outweighs it");
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_WRONG_RESOLUTION, context.dueIndex(), 1);
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_INVALID, context.dueIndex(), 2);
    s = statuses(&before, context.snapshot());
    CHECK(s.stream.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.stream.message} == "2 grains invalid");

    INFO("a missing grain was lost on the way, not decoded at all");
    auto const after = context.snapshot();
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_MISSING, context.dueIndex(), 1);
    s = statuses(&after, context.snapshot());
    CHECK(s.stream.status == MXL_MONITORING_BCP008_UNCHANGED);
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
}

TEST_CASE("A writer committing at the head is a healthy sender", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open(MXL_MONITORING_ROLE_WRITER);
    keepUp(context, flow, 3);
    auto const before = context.snapshot();
    keepUp(context, flow, 3);
    auto const s = statuses(&before, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_HEALTHY);
    CHECK(s.stream.status == MXL_MONITORING_BCP008_HEALTHY);
}

TEST_CASE("A failed commit makes the transmission unhealthy, late input only the essence", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open(MXL_MONITORING_ROLE_WRITER);
    keepUp(context, flow, 3);
    auto const before = context.snapshot();

    mxlMonitoringRecordWriteOutcome(flow, MXL_MONITORING_WRITE_INPUT_LATE, MXL_STATUS_OK, context.dueIndex(), 2);
    keepUp(context, flow, 1);
    auto s = statuses(&before, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_HEALTHY);
    CHECK(s.stream.status == MXL_MONITORING_BCP008_PARTIALLY_HEALTHY);
    CHECK(std::string{s.stream.message} == "2 grains without input in time");

    INFO("a writer with no input at all has no essence");
    auto const some = context.snapshot();
    mxlMonitoringRecordWriteOutcome(flow, MXL_MONITORING_WRITE_INPUT_LATE, MXL_STATUS_OK, context.dueIndex(), 1);
    s = statuses(&some, context.snapshot());
    CHECK(s.stream.status == MXL_MONITORING_BCP008_UNHEALTHY);

    mxlMonitoringRecordWriteContent(flow, MXL_MONITORING_CONTENT_WRONG_RESOLUTION, context.dueIndex(), 1);
    s = statuses(&before, context.snapshot());
    CHECK(s.stream.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.stream.message} == "1 grain of invalid input, wrong_resolution");

    auto const late = context.snapshot();
    mxlMonitoringRecordWrite(flow, MXL_ERR_UNKNOWN, context.dueIndex(), 1);
    INFO("the failure leaves the writer unusable until a commit lands");
    s = statuses(&late, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.connection.message}.starts_with("flow flow_error"));
    keepUp(context, flow, 1);
    s = statuses(&late, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.connection.message} == "1 failed commit: 1 write_error");
}

TEST_CASE("A sender that commits nothing is unhealthy, and its essence unjudged", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open(MXL_MONITORING_ROLE_WRITER);
    keepUp(context, flow, 3);
    auto const before = context.snapshot();
    context.now += 2 * Second;
    auto const s = statuses(&before, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.connection.message} == "nothing committed within the last second");
    CHECK(s.stream.status == MXL_MONITORING_BCP008_UNCHANGED);
}

TEST_CASE("A count below the previous snapshot's, from a process started again, counts from zero", "[bcp008]")
{
    // A Node polling another process's counts sees them restart.
    auto context = Context{};
    auto const flow = context.open(MXL_MONITORING_ROLE_WRITER);
    keepUp(context, flow, 3);
    auto restarted = context.snapshot();
    auto previous = restarted;
    previous.writes[MXL_MONITORING_WRITE_INPUT_LATE] = 9;
    restarted.writes[MXL_MONITORING_WRITE_INPUT_LATE] = 2;
    auto const s = statuses(&previous, restarted);
    CHECK(s.stream.status == MXL_MONITORING_BCP008_PARTIALLY_HEALTHY);
    CHECK(std::string{s.stream.message} == "2 grains without input in time");
}

TEST_CASE("Statuses and counters refuse a previous snapshot of another flow", "[bcp008]")
{
    auto context = Context{};
    context.open();
    context.open(MXL_MONITORING_ROLE_WRITER);
    auto const reader = context.snapshot(0);
    auto const writer = context.snapshot(1);
    auto out = mxlMonitoringBcp008Statuses{};
    out.version = 1;
    out.size = sizeof(out);
    CHECK(mxlMonitoringBcp008GetStatuses(&reader, &writer, &out) == MXL_ERR_INVALID_ARG);
    auto count = std::size_t{0};
    CHECK(mxlMonitoringBcp008GetCounters(&reader, &writer, MXL_MONITORING_BCP008_TRANSMISSION_ERROR, nullptr, &count) ==
          MXL_ERR_INVALID_ARG);
    out.size = 8;
    CHECK(mxlMonitoringBcp008GetStatuses(nullptr, &writer, &out) == MXL_ERR_INVALID_ARG);
}

TEST_CASE("A receiver's lost and late counters count since the last reset", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordSkipped(flow, 3);
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_MISSING, context.dueIndex(), 1);
    CHECK(counters(nullptr, context.snapshot(), MXL_MONITORING_BCP008_LOST) == std::vector<Named>{{"skipped", 3}, {"missing", 1}});

    auto const reset = context.snapshot();
    mxlMonitoringRecordSkipped(flow, 2);
    auto const index = context.dueIndex() + 1;
    context.now += 100 * Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
    CHECK(counters(&reset, context.snapshot(), MXL_MONITORING_BCP008_LOST) == std::vector<Named>{{"skipped", 2}, {"missing", 0}});
    CHECK(counters(&reset, context.snapshot(), MXL_MONITORING_BCP008_LATE) == std::vector<Named>{{"too_early", 1}});
    INFO("a receiver has no transmission errors, which BCP-008 answers with an empty collection");
    CHECK(counters(&reset, context.snapshot(), MXL_MONITORING_BCP008_TRANSMISSION_ERROR).empty());
}

TEST_CASE("A sender's transmission errors are its failed commits, not its input", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open(MXL_MONITORING_ROLE_WRITER);
    mxlMonitoringRecordWrite(flow, MXL_ERR_PERMISSION_DENIED, context.dueIndex(), 1);
    mxlMonitoringRecordWriteOutcome(flow, MXL_MONITORING_WRITE_OUT_OF_SPACE, MXL_ERR_UNKNOWN, context.dueIndex(), 1);
    mxlMonitoringRecordWrite(flow, MXL_ERR_UNKNOWN, context.dueIndex(), 1);
    mxlMonitoringRecordWriteOutcome(flow, MXL_MONITORING_WRITE_INPUT_LATE, MXL_STATUS_OK, context.dueIndex(), 5);
    CHECK(counters(nullptr, context.snapshot(), MXL_MONITORING_BCP008_TRANSMISSION_ERROR) ==
          std::vector<Named>{{"permission_denied", 1}, {"out_of_space", 1}, {"write_error", 1}});
    CHECK(counters(nullptr, context.snapshot(), MXL_MONITORING_BCP008_LOST).empty());

    INFO("a buffer too small is filled as far as it goes");
    auto one = mxlMonitoringBcp008Counter{};
    auto count = std::size_t{1};
    auto const s = context.snapshot();
    REQUIRE(mxlMonitoringBcp008GetCounters(nullptr, &s, MXL_MONITORING_BCP008_TRANSMISSION_ERROR, &one, &count) == MXL_STATUS_OK);
    CHECK(count == 3);
    CHECK(std::string{one.name} == "permission_denied");
}

TEST_CASE("A failure the receiver recovered from before the poll still counts against the connection", "[bcp008]")
{
    // The flow is open and delivering again by the time the Node polls, so
    // only the counts tell that something went wrong.
    auto context = Context{};
    auto const flow = context.open();
    keepUp(context, flow, 3);

    auto before = context.snapshot();
    mxlMonitoringRecordRead(flow, MXL_ERR_UNKNOWN, context.dueIndex(), 1);
    keepUp(context, flow, 1);
    auto s = statuses(&before, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.connection.message} == "1 failed read");

    before = context.snapshot();
    mxlMonitoringRecordRead(flow, MXL_ERR_FLOW_NOT_FOUND, context.dueIndex(), 1);
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
    keepUp(context, flow, 1);
    s = statuses(&before, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.connection.message} == "flow went away 1 time");

    before = context.snapshot();
    auto const index = context.dueIndex() + 1;
    context.now += 300 * Ms; // past the 100 ms stall timeout, within the second
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, index, 1);
    s = statuses(&before, context.snapshot());
    REQUIRE(context.snapshot().delivering);
    CHECK(s.connection.status == MXL_MONITORING_BCP008_UNHEALTHY);
    CHECK(std::string{s.connection.message} == "1 stall");
}

TEST_CASE("Replication sources are judged as receivers, targets as senders", "[bcp008]")
{
    auto context = Context{};
    auto const source = context.open(MXL_MONITORING_ROLE_REPLICATION_SOURCE);
    auto const target = context.open(MXL_MONITORING_ROLE_REPLICATION_TARGET);
    mxlMonitoringRecordSkipped(source, 2);
    mxlMonitoringRecordWrite(target, MXL_ERR_UNKNOWN, context.dueIndex(), 1);
    CHECK(counters(nullptr, context.snapshot(0), MXL_MONITORING_BCP008_LOST) == std::vector<Named>{{"skipped", 2}, {"missing", 0}});
    CHECK(counters(nullptr, context.snapshot(1), MXL_MONITORING_BCP008_TRANSMISSION_ERROR) ==
          std::vector<Named>{{"permission_denied", 0}, {"out_of_space", 0}, {"write_error", 1}});
}

TEST_CASE("A first judgement rests on access and delivery alone", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordSkipped(flow, 5);
    keepUp(context, flow, 2);
    auto const s = statuses(nullptr, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_HEALTHY);
    CHECK(s.stream.status == MXL_MONITORING_BCP008_UNCHANGED);
}

TEST_CASE("A disconnected sender is inactive", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open(MXL_MONITORING_ROLE_WRITER);
    REQUIRE(mxlMonitoringSetFlowTarget(flow, "") == MXL_STATUS_OK);
    auto const s = statuses(nullptr, context.snapshot());
    CHECK(s.connection.status == MXL_MONITORING_BCP008_INACTIVE);
    CHECK(s.stream.status == MXL_MONITORING_BCP008_INACTIVE);
}

TEST_CASE("Messages name samples for a continuous flow, and the format a flow differs in", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open();
    keepUp(context, flow, 2);
    auto previous = context.snapshot();
    auto current = previous;
    current.unit = MXL_MONITORING_UNIT_SAMPLE;
    current.skipped += 480;
    current.content[MXL_MONITORING_CONTENT_WRONG_RATE] += 1;
    std::snprintf(current.formatMismatch, sizeof(current.formatMismatch), "sample rate 44100/1, expected 48000/1");
    auto const s = statuses(&previous, current);
    CHECK(std::string{s.connection.message} == "480 samples lost");
    CHECK(std::string{s.stream.message} == "1 sample wrong_rate: sample rate 44100/1, expected 48000/1");
}

TEST_CASE("A long message is cut to the field without splitting a character", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.add();
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OTHER, MXL_ERR_UNKNOWN);
    auto current = context.snapshot();
    // "flow flow_error: " is 17 bytes, so the message's byte 255, the first
    // one past the field, is the second byte of the first two-byte
    // character: cutting there would leave its first byte dangling.
    auto detail = std::string(237, 'x');
    while (detail.size() < 253)
    {
        detail += "\xc3\xa9";
    }
    std::snprintf(current.lastErrorDetail, sizeof(current.lastErrorDetail), "%s", detail.c_str());
    auto const message = std::string{statuses(nullptr, current).connection.message};
    CHECK(message.size() == MXL_MONITORING_TEXT - 2);
    CHECK(message.back() == 'x');
}

TEST_CASE("A restart between polls is detected, and counters taken past an old baseline stay right", "[bcp008]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordSkipped(flow, 100);
    keepUp(context, flow, 2);
    auto const baseline = context.snapshot();
    CHECK_FALSE(mxlMonitoringBcp008Restarted(&baseline, &baseline));

    auto restarted = baseline;
    restarted.opens = 1;
    restarted.skipped = 5;
    CHECK(mxlMonitoringBcp008Restarted(&baseline, &restarted));
    CHECK(counters(&baseline, restarted, MXL_MONITORING_BCP008_LOST)[0] == Named{"skipped", 5});

    INFO("an interval whose counts are partly above the earlier ones is all new");
    restarted.reads[MXL_MONITORING_READ_TOO_EARLY] = baseline.reads[MXL_MONITORING_READ_TOO_EARLY] + 1;
    auto const s = statuses(&baseline, restarted);
    CHECK(std::string{s.connection.message} == "5 grains lost");
    CHECK(counters(&baseline, restarted, MXL_MONITORING_BCP008_LATE)[0] == Named{"too_early", restarted.reads[MXL_MONITORING_READ_TOO_EARLY]});
}

TEST_CASE("Statuses and counters refuse what is not a snapshot of the flow", "[bcp008]")
{
    auto context = Context{};
    context.open();
    context.open(MXL_MONITORING_ROLE_READER, "d4d00000-0000-0000-0000-000000000002");
    auto const one = context.snapshot(0);
    auto const other = context.snapshot(1);
    auto out = mxlMonitoringBcp008Statuses{};
    out.version = 1;
    out.size = sizeof(out);
    CHECK(mxlMonitoringBcp008GetStatuses(&one, &other, &out) == MXL_ERR_INVALID_ARG);

    auto old = one;
    old.version = 1;
    CHECK(mxlMonitoringBcp008GetStatuses(nullptr, &old, &out) == MXL_ERR_INVALID_ARG);
    out.version = 0;
    CHECK(mxlMonitoringBcp008GetStatuses(nullptr, &one, &out) == MXL_ERR_INVALID_ARG);

    auto count = std::size_t{1};
    CHECK(mxlMonitoringBcp008GetCounters(nullptr, &one, MXL_MONITORING_BCP008_LOST, nullptr, &count) == MXL_ERR_INVALID_ARG);
}
