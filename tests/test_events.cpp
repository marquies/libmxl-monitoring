// Notification of errors: one event when a condition starts, a summary while
// it persists, one when it is over. Not one per grain: at 50 grains a second
// that would bury the condition it reports.

#include <catch2/catch_test_macros.hpp>
#include "Support.hpp"

using namespace mxl::monitoring::tests;

namespace
{
    void deliver(Context& context, mxlMonitoringFlow flow, int grains)
    {
        for (int i = 0; i < grains; ++i)
        {
            context.now += 20 * Ms;
            mxlMonitoringRecordRead(flow, MXL_STATUS_OK, context.dueIndex(), 1);
        }
    }
}

TEST_CASE("A read condition starts, persists and recovers", "[events]")
{
    auto context = Context{};
    auto const flow = context.open();
    REQUIRE(mxlMonitoringPoll(context.monitoring) == MXL_STATUS_OK);
    CHECK(context.events.empty());

    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 70, 1);
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 71, 1);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 1);
    auto const& started = context.events[0];
    CHECK(std::string{started.flowId} == FlowId);
    CHECK(started.kind == MXL_MONITORING_CONDITION_READ);
    CHECK(started.read == MXL_MONITORING_READ_TOO_LATE);
    CHECK(started.phase == MXL_MONITORING_PHASE_STARTED);
    CHECK(started.status == MXL_ERR_OUT_OF_RANGE_TOO_LATE);
    CHECK(started.index == 71);
    CHECK(started.occurrences == 2);
    CHECK(started.content == MXL_MONITORING_CONTENT_COUNT);

    INFO("recurring within the interval adds no event");
    for (int i = 0; i < 59; ++i)
    {
        context.now += Second;
        mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 80 + i, 1);
        mxlMonitoringPoll(context.monitoring);
    }
    CHECK(context.events.size() == 1);
    context.now += Second;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 200, 1);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 2);
    CHECK(context.events[1].phase == MXL_MONITORING_PHASE_PERSISTING);
    CHECK(context.events[1].occurrences == 60);
    CHECK(context.events[1].total == 62);
    CHECK(context.events[1].durationNs == 60 * Second);

    deliver(context, flow, 25);
    mxlMonitoringPoll(context.monitoring);
    CHECK(context.events.size() == 2);
    deliver(context, flow, 25);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 3);
    CHECK(context.events[2].phase == MXL_MONITORING_PHASE_RECOVERED);
    CHECK(context.events[2].total == 62);
    CHECK(context.events[2].occurrences == 0);
}

TEST_CASE("A flow that only went quiet has not recovered", "[events]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 70, 1);
    mxlMonitoringPoll(context.monitoring);
    context.now += 10 * Second;
    mxlMonitoringPoll(context.monitoring);
    CHECK(context.events.size() == 1);
    INFO("one grain, then quiet again, is not delivering");
    deliver(context, flow, 1);
    context.now += Second;
    mxlMonitoringPoll(context.monitoring);
    CHECK(context.events.size() == 1);
    deliver(context, flow, 50);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 2);
    CHECK(context.events[1].phase == MXL_MONITORING_PHASE_RECOVERED);
}

TEST_CASE("A flow delivering only bad content has not recovered", "[events]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 70, 1);
    mxlMonitoringPoll(context.monitoring);
    for (int i = 0; i < 100; ++i)
    {
        context.now += 20 * Ms;
        mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_INCOMPLETE, context.dueIndex(), 1);
    }
    mxlMonitoringPoll(context.monitoring);
    for (auto const& event : context.events)
    {
        CHECK_FALSE((event.read == MXL_MONITORING_READ_TOO_LATE && event.phase == MXL_MONITORING_PHASE_RECOVERED));
    }
}

TEST_CASE("A flow left without data keeps reporting its condition", "[events]")
{
    auto context = Context{};
    auto const flow = context.open();
    deliver(context, flow, 5);
    INFO("a reader waiting for a writer that stopped meets one too-early index");
    auto const index = context.dueIndex() + 1;
    context.now += 200 * Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 1);
    CHECK(context.events[0].read == MXL_MONITORING_READ_TOO_EARLY);
    for (int i = 0; i < 120; ++i)
    {
        context.now += Second;
        mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
        mxlMonitoringPoll(context.monitoring);
    }
    REQUIRE(context.events.size() == 3);
    CHECK(context.events[1].phase == MXL_MONITORING_PHASE_PERSISTING);
    CHECK(context.events[1].occurrences == 0);
    CHECK(context.events[2].durationNs == 120 * Second);
}

TEST_CASE("An access condition ends when the flow is open", "[events]")
{
    auto context = Context{};
    auto const flow = context.add();
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_FLOW_NOT_FOUND, MXL_ERR_FLOW_NOT_FOUND);
    mxlMonitoringPoll(context.monitoring);
    for (int i = 0; i < 5; ++i)
    {
        context.now += Second;
        mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_FLOW_NOT_FOUND, MXL_ERR_FLOW_NOT_FOUND);
        mxlMonitoringPoll(context.monitoring);
    }
    REQUIRE(context.events.size() == 1);
    CHECK(context.events[0].kind == MXL_MONITORING_CONDITION_ACCESS);
    CHECK(context.events[0].access == MXL_MONITORING_ACCESS_FLOW_NOT_FOUND);

    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 2);
    CHECK(context.events[1].phase == MXL_MONITORING_PHASE_RECOVERED);
    CHECK(context.events[1].total == 6);
    CHECK(context.events[1].durationNs == 5 * Second);
}

TEST_CASE("A flow lost under its reader starts an access condition", "[events]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordRead(flow, MXL_ERR_FLOW_INVALID, 9, 1);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 1);
    CHECK(context.events[0].kind == MXL_MONITORING_CONDITION_ACCESS);
    CHECK(context.events[0].access == MXL_MONITORING_ACCESS_FLOW_INVALID);
    CHECK(context.events[0].status == MXL_ERR_FLOW_INVALID);
}

TEST_CASE("Content issues notify separately", "[events]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_WRONG_FORMAT, 1, 1);
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_ILLEGAL_VALUES, 2, 1);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 2);
    for (auto const& event : context.events)
    {
        CHECK(event.kind == MXL_MONITORING_CONDITION_READ);
        CHECK(event.read == MXL_MONITORING_READ_INVALID_CONTENT);
        CHECK(event.phase == MXL_MONITORING_PHASE_STARTED);
    }
    CHECK(context.events[0].content == MXL_MONITORING_CONTENT_WRONG_FORMAT);
    CHECK(context.events[1].content == MXL_MONITORING_CONTENT_ILLEGAL_VALUES);
    CHECK(context.events[1].index == 2);
}

TEST_CASE("Writer conditions are write conditions", "[events]")
{
    auto context = Context{};
    auto const flow = context.open(MXL_MONITORING_ROLE_WRITER);
    mxlMonitoringRecordWriteOutcome(flow, MXL_MONITORING_WRITE_INPUT_LATE, MXL_STATUS_OK, 4, 2);
    mxlMonitoringRecordWriteContent(flow, MXL_MONITORING_CONTENT_WRONG_SIZE, 5, 1);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 2);
    CHECK(context.events[0].kind == MXL_MONITORING_CONDITION_WRITE);
    CHECK(context.events[0].write == MXL_MONITORING_WRITE_INPUT_LATE);
    CHECK(context.events[0].occurrences == 2);
    CHECK(context.events[1].write == MXL_MONITORING_WRITE_INVALID_INPUT);
    CHECK(context.events[1].content == MXL_MONITORING_CONTENT_WRONG_SIZE);
}

TEST_CASE("Disconnecting ends an access condition and silences a flow without data", "[events]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 3, 1);
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_FLOW_NOT_FOUND, MXL_ERR_FLOW_NOT_FOUND);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 2);

    REQUIRE(mxlMonitoringSetFlowTarget(flow, "") == MXL_STATUS_OK);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 3);
    CHECK(context.events[2].kind == MXL_MONITORING_CONDITION_ACCESS);
    CHECK(context.events[2].phase == MXL_MONITORING_PHASE_RECOVERED);

    INFO("a slot disconnected on purpose delivering nothing is not a fault");
    context.now += 120 * Second;
    mxlMonitoringPoll(context.monitoring);
    CHECK(context.events.size() == 3);
}

TEST_CASE("A caller recording by hand describes its own conditions", "[events]")
{
    auto context = Context{};
    auto const flow = context.open();
    REQUIRE(mxlMonitoringDescribe(flow, MXL_MONITORING_CONDITION_READ, MXL_MONITORING_READ_INVALID_CONTENT,
                MXL_MONITORING_CONTENT_ILLEGAL_VALUES, "v210 code 0 at row 1") == MXL_STATUS_OK);
    REQUIRE(mxlMonitoringDescribe(flow, MXL_MONITORING_CONDITION_READ, MXL_MONITORING_READ_ERROR, MXL_MONITORING_CONTENT_COUNT,
                "read failed: status 8") == MXL_STATUS_OK);
    CHECK(mxlMonitoringDescribe(flow, MXL_MONITORING_CONDITION_READ, MXL_MONITORING_READ_INVALID_CONTENT, MXL_MONITORING_CONTENT_COUNT, "x") ==
          MXL_ERR_INVALID_ARG);
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_ILLEGAL_VALUES, 1, 1);
    mxlMonitoringRecordRead(flow, MXL_ERR_INVALID_ARG, 2, 1);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 2);
    for (auto const& event : context.events)
    {
        if (event.read == MXL_MONITORING_READ_ERROR)
        {
            CHECK(std::string{event.detail} == "read failed: status 8");
        }
        else
        {
            CHECK(std::string{event.detail} == "v210 code 0 at row 1");
        }
    }
}
