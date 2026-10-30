// What recording does to a flow's counts and state. The clock is the test's,
// so every timing rule is exercised at a known distance from a grain's media
// timestamp.

#include <catch2/catch_test_macros.hpp>
#include "Support.hpp"

using namespace mxl::monitoring::tests;

TEST_CASE("A flow is known before it is opened, and only once per role", "[recording]")
{
    auto context = Context{};
    auto const flow = context.add();
    auto const s = context.snapshot();
    CHECK(std::string{s.flowId} == FlowId);
    CHECK(s.access == MXL_MONITORING_ACCESS_STARTING);
    CHECK(s.unit == MXL_MONITORING_UNIT_UNKNOWN);

    auto again = mxlMonitoringFlow{};
    CHECK(mxlMonitoringAddFlow(context.monitoring, FlowId, MXL_MONITORING_ROLE_READER, &again) == MXL_ERR_EXISTS);
    CHECK(mxlMonitoringAddFlow(context.monitoring, FlowId, MXL_MONITORING_ROLE_WRITER, &again) == MXL_STATUS_OK);
    CHECK(mxlMonitoringAddFlow(context.monitoring, "", MXL_MONITORING_ROLE_READER, &again) == MXL_ERR_INVALID_ARG);

    REQUIRE(mxlMonitoringRemoveFlow(context.monitoring, flow) == MXL_STATUS_OK);
    auto count = std::size_t{0};
    REQUIRE(mxlMonitoringSnapshot(context.monitoring, nullptr, &count) == MXL_STATUS_OK);
    CHECK(count == 1);
}

TEST_CASE("Failed attempts count by reason, and opening ends them", "[recording]")
{
    auto context = Context{};
    auto const flow = context.add();
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE, MXL_ERR_UNKNOWN);
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_FLOW_NOT_FOUND, MXL_ERR_FLOW_NOT_FOUND);
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_FLOW_NOT_FOUND, MXL_ERR_FLOW_NOT_FOUND);
    auto s = context.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_FLOW_NOT_FOUND);
    CHECK(s.accessFailures[MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE] == 1);
    CHECK(s.accessFailures[MXL_MONITORING_ACCESS_FLOW_NOT_FOUND] == 2);
    CHECK(s.lastErrorStatus == MXL_ERR_FLOW_NOT_FOUND);
    CHECK(s.opens == 0);

    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
    s = context.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_OPEN);
    CHECK(s.opens == 1);
    CHECK(mxlMonitoringFlowLastProgress(flow) == context.now);
}

TEST_CASE("A good read counts its units, its age and its arrival latency", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    auto const index = context.dueIndex();
    context.now += 3 * Ms;
    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, index, 1);
    context.now += 500 * Ms;
    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, index + 1, 1);

    auto const s = context.snapshot();
    CHECK(s.reads[MXL_MONITORING_READ_OK] == 2);
    CHECK(s.lastIndex == index + 1);
    CHECK(s.ageNs == static_cast<std::int64_t>(483 * Ms));
    CHECK(s.delivering);
    CHECK(s.arrivalLatency.count == 2);
    CHECK(s.arrivalLatency.buckets[2] == 1); // 3 ms: above 2 ms, within 5 ms
    CHECK(s.arrivalLatency.buckets[9] == 1); // 483 ms: above 320 ms, within 640 ms
    CHECK(s.arrivalLatency.sumNs == 486 * Ms);

    context.now += Second;
    CHECK_FALSE(context.snapshot().delivering);
}

TEST_CASE("A latency on a bucket bound falls in that bucket, as Prometheus le is inclusive", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    auto const index = context.dueIndex();
    context.now += 20 * Ms;
    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, index, 1);
    auto const s = context.snapshot();
    CHECK(s.arrivalLatency.buckets[4] == 1); // le 0.02
    CHECK(s.arrivalLatency.buckets[5] == 0);
}

TEST_CASE("Too early counts once per index, and only past the grace", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    auto const index = context.dueIndex() + 1; // due in 20 ms, grace 70 ms

    INFO("a reader at the head finds nothing until the next grain is due");
    for (int i = 0; i < 9; ++i)
    {
        context.now += 10 * Ms;
        mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
    }
    CHECK(context.snapshot().reads[MXL_MONITORING_READ_TOO_EARLY] == 0);

    context.now += 10 * Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
    mxlMonitoringRecordRead(flow, MXL_ERR_TIMEOUT, index, 1);
    CHECK(context.snapshot().reads[MXL_MONITORING_READ_TOO_EARLY] == 1);

    context.now += 20 * Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index + 1, 1);
    CHECK(context.snapshot().reads[MXL_MONITORING_READ_TOO_EARLY] == 2);
}

TEST_CASE("A stall counts once per timeout without data", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open(); // stall after 100 ms
    auto const index = context.dueIndex();
    context.now += 99 * Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
    CHECK(context.snapshot().stalls == 0);
    context.now += Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
    context.now += 300 * Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
    CHECK(context.snapshot().stalls == 1);

    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, index, 1);
    context.now += 100 * Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index + 1, 1);
    CHECK(context.snapshot().stalls == 2);
}

TEST_CASE("Too late counts the overtaking, skipped the grains lost to it", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 100, 1);
    mxlMonitoringRecordSkipped(flow, 12);
    auto const s = context.snapshot();
    CHECK(s.reads[MXL_MONITORING_READ_TOO_LATE] == 1);
    CHECK(s.skipped == 12);
    CHECK(s.lastErrorIndex == 100);
    CHECK(s.access == MXL_MONITORING_ACCESS_OPEN);
}

TEST_CASE("A read that cannot be done leaves the flow unusable until data moves", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordRead(flow, MXL_ERR_PERMISSION_DENIED, 5, 1);
    auto s = context.snapshot();
    CHECK(s.reads[MXL_MONITORING_READ_PERMISSION_DENIED] == 1);
    CHECK(s.access == MXL_MONITORING_ACCESS_PERMISSION_DENIED);
    CHECK_FALSE(s.delivering);

    mxlMonitoringRecordRead(flow, MXL_ERR_INVALID_ARG, 5, 1);
    s = context.snapshot();
    CHECK(s.reads[MXL_MONITORING_READ_ERROR] == 1);
    CHECK(s.access == MXL_MONITORING_ACCESS_OTHER);

    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, 6, 1);
    CHECK(context.snapshot().access == MXL_MONITORING_ACCESS_OPEN);
}

TEST_CASE("A flow that goes away under a reader is a loss, not a read error", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordRead(flow, MXL_ERR_FLOW_INVALID, 5, 1);
    auto s = context.snapshot();
    CHECK(s.losses == 1);
    CHECK(s.access == MXL_MONITORING_ACCESS_FLOW_INVALID);
    CHECK(s.reads[MXL_MONITORING_READ_ERROR] == 0);
    INFO("a loss is not a failed attempt to open");
    CHECK(s.accessFailures[MXL_MONITORING_ACCESS_FLOW_INVALID] == 0);

    INFO("retrying the read before reopening loses the flow only once");
    mxlMonitoringRecordRead(flow, MXL_ERR_FLOW_NOT_FOUND, 5, 1);
    s = context.snapshot();
    CHECK(s.losses == 1);
    CHECK(s.access == MXL_MONITORING_ACCESS_FLOW_NOT_FOUND);

    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
    mxlMonitoringRecordRead(flow, MXL_ERR_INVALID_FLOW_READER, 6, 1);
    CHECK(context.snapshot().losses == 2);
}

TEST_CASE("Only the statuses that mean the flow went away are losses", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordRead(flow, MXL_ERR_NOT_FOUND, 5, 1);
    auto const s = context.snapshot();
    CHECK(s.losses == 0);
    CHECK(s.reads[MXL_MONITORING_READ_ERROR] == 1);
}

TEST_CASE("Wrong content counts instead of OK, never as both", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_WRONG_FORMAT, 1, 480);
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_MISSING, 2, 1);
    auto const s = context.snapshot();
    CHECK(s.reads[MXL_MONITORING_READ_OK] == 0);
    CHECK(s.reads[MXL_MONITORING_READ_INVALID_CONTENT] == 481);
    CHECK(s.content[MXL_MONITORING_CONTENT_WRONG_FORMAT] == 480);
    CHECK(s.content[MXL_MONITORING_CONTENT_MISSING] == 1);
    INFO("data arrived, so the flow delivers");
    CHECK(s.delivering);
}

TEST_CASE("Writes count commits, the outcomes only the writer sees, and their latencies", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open(MXL_MONITORING_ROLE_WRITER);
    auto const index = context.dueIndex();
    mxlMonitoringRecordInput(flow, index, context.now + 4 * Ms);
    context.now += 8 * Ms;
    mxlMonitoringRecordWrite(flow, MXL_STATUS_OK, index, 1);
    mxlMonitoringRecordWriteOutcome(flow, MXL_MONITORING_WRITE_INPUT_LATE, MXL_STATUS_OK, index + 1, 3);
    mxlMonitoringRecordWriteOutcome(flow, MXL_MONITORING_WRITE_OUT_OF_SPACE, MXL_ERR_UNKNOWN, index + 4, 1);
    mxlMonitoringRecordWriteContent(flow, MXL_MONITORING_CONTENT_WRONG_RESOLUTION, index + 5, 1);
    mxlMonitoringRecordWrite(flow, MXL_ERR_PERMISSION_DENIED, index + 6, 1);

    auto const s = context.snapshot();
    CHECK(s.writes[MXL_MONITORING_WRITE_OK] == 1);
    CHECK(s.writes[MXL_MONITORING_WRITE_INPUT_LATE] == 3);
    CHECK(s.writes[MXL_MONITORING_WRITE_OUT_OF_SPACE] == 1);
    CHECK(s.writes[MXL_MONITORING_WRITE_INVALID_INPUT] == 1);
    CHECK(s.writes[MXL_MONITORING_WRITE_PERMISSION_DENIED] == 1);
    CHECK(s.content[MXL_MONITORING_CONTENT_WRONG_RESOLUTION] == 1);
    CHECK(s.access == MXL_MONITORING_ACCESS_PERMISSION_DENIED);
    CHECK(s.commitLatency.count == 1);
    CHECK(s.commitLatency.sumNs == 8 * Ms);
    CHECK(s.inputLatency.sumNs == 4 * Ms);
}

TEST_CASE("Latency needs a rate, and a clock behind the timestamp counts as 0", "[recording]")
{
    auto context = Context{};
    auto const flow = context.add();
    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, 3, 1);
    auto const none = context.snapshot();
    CHECK(none.ageNs == 0);
    INFO("no latency is observed as if it were 0");
    CHECK(none.arrivalLatency.count == 0);
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, 4, 1);
    INFO("nothing says when an index is due without a rate");
    CHECK(context.snapshot().reads[MXL_MONITORING_READ_TOO_EARLY] == 0);

    auto const open = context.open(MXL_MONITORING_ROLE_READER, "d4d00000-0000-0000-0000-000000000002");
    mxlMonitoringRecordRead(open, MXL_STATUS_OK, context.dueIndex() + 10, 1);
    auto const s = context.snapshot(1);
    CHECK(s.ageNs < 0);
    CHECK(s.arrivalLatency.buckets[0] == 1);
    CHECK(s.arrivalLatency.sumNs == 0);
}

TEST_CASE("Bad arguments are refused rather than recorded", "[recording]")
{
    auto context = Context{};
    auto const flow = context.add();
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_COUNT, MXL_STATUS_OK);
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_COUNT, 0, 1);
    mxlMonitoringRecordRead(nullptr, MXL_STATUS_OK, 0, 1);
    auto const s = context.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_STARTING);
    CHECK(s.reads[MXL_MONITORING_READ_INVALID_CONTENT] == 0);

    auto config = mxlMonitoringConfig{};
    auto other = mxlMonitoring{};
    CHECK(mxlMonitoringCreate(&config, &other) == MXL_ERR_INVALID_ARG);
    auto format = mxlMonitoringFlowFormat{};
    CHECK(mxlMonitoringSetFlowFormat(flow, &format) == MXL_ERR_INVALID_ARG);
}

TEST_CASE("Retargeting keeps the counts and forgets the flow left", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, context.dueIndex(), 1);
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 3, 1);
    REQUIRE(mxlMonitoringSetFlowTarget(flow, "d4d00000-0000-0000-0000-0000000000ff") == MXL_STATUS_OK);

    auto s = context.snapshot();
    CHECK(std::string{s.flowId} == FlowId);
    CHECK(std::string{s.target} == "d4d00000-0000-0000-0000-0000000000ff");
    CHECK(s.access == MXL_MONITORING_ACCESS_STARTING);
    CHECK(s.reads[MXL_MONITORING_READ_OK] == 1);
    CHECK(s.reads[MXL_MONITORING_READ_TOO_LATE] == 1);
    CHECK(s.opens == 1);
    CHECK(std::string{s.mediaType}.empty());
    CHECK(s.unit == MXL_MONITORING_UNIT_UNKNOWN);
    CHECK(s.lastDataTimeNs == 0);
    CHECK(s.lastErrorTimeNs == 0);
    CHECK_FALSE(s.delivering);

    REQUIRE(mxlMonitoringSetFlowTarget(flow, nullptr) == MXL_STATUS_OK);
    s = context.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_DISCONNECTED);
    CHECK(std::string{s.target}.empty());
    CHECK(std::string{mxlMonitoringAccessName(s.access)} == "disconnected");
}

TEST_CASE("Pointing a flow at the flow it already has changes nothing", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, context.dueIndex(), 1);
    REQUIRE(mxlMonitoringSetFlowTarget(flow, FlowId) == MXL_STATUS_OK);
    auto const s = context.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_OPEN);
    CHECK(std::string{s.mediaType} == "video/v210");
    CHECK(s.delivering);
}

TEST_CASE("Ids are names up to the length a snapshot carries", "[recording]")
{
    auto context = Context{};
    auto flow = mxlMonitoringFlow{};
    auto const longest = std::string(MXL_MONITORING_ID - 1, 'x');
    CHECK(mxlMonitoringAddFlow(context.monitoring, longest.c_str(), MXL_MONITORING_ROLE_READER, &flow) == MXL_STATUS_OK);
    CHECK(std::string{context.snapshot().flowId} == longest);
    auto const tooLong = std::string(MXL_MONITORING_ID, 'x');
    CHECK(mxlMonitoringAddFlow(context.monitoring, tooLong.c_str(), MXL_MONITORING_ROLE_WRITER, &flow) == MXL_ERR_INVALID_ARG);
}

TEST_CASE("Every read and write outcome and content issue has its OTLP name", "[recording]")
{
    auto const read = [](mxlMonitoringRead r) { return std::string{mxlMonitoringReadName(r)}; };
    CHECK(read(MXL_MONITORING_READ_OK) == "ok");
    CHECK(read(MXL_MONITORING_READ_TOO_EARLY) == "too_early");
    CHECK(read(MXL_MONITORING_READ_TOO_LATE) == "too_late");
    CHECK(read(MXL_MONITORING_READ_PERMISSION_DENIED) == "permission_denied");
    CHECK(read(MXL_MONITORING_READ_ERROR) == "read_error");
    CHECK(read(MXL_MONITORING_READ_INVALID_CONTENT) == "invalid_content");
    CHECK(read(MXL_MONITORING_READ_COUNT) == "unknown");
    auto const write = [](mxlMonitoringWrite w) { return std::string{mxlMonitoringWriteName(w)}; };
    CHECK(write(MXL_MONITORING_WRITE_OK) == "ok");
    CHECK(write(MXL_MONITORING_WRITE_INPUT_LATE) == "input_late");
    CHECK(write(MXL_MONITORING_WRITE_INVALID_INPUT) == "invalid_input");
    CHECK(write(MXL_MONITORING_WRITE_PERMISSION_DENIED) == "permission_denied");
    CHECK(write(MXL_MONITORING_WRITE_OUT_OF_SPACE) == "out_of_space");
    CHECK(write(MXL_MONITORING_WRITE_ERROR) == "write_error");
    CHECK(write(MXL_MONITORING_WRITE_COUNT) == "unknown");
    CHECK(std::string{mxlMonitoringContentName(MXL_MONITORING_CONTENT_WRONG_FORMAT)} == "wrong_format");
    CHECK(std::string{mxlMonitoringContentName(MXL_MONITORING_CONTENT_ILLEGAL_VALUES)} == "illegal_values");
    INFO("the count stands for no issue, which has no name");
    CHECK(mxlMonitoringContentName(MXL_MONITORING_CONTENT_COUNT) == nullptr);
}

TEST_CASE("Every access state has mxl-reader's name", "[recording]")
{
    CHECK(std::string{mxlMonitoringAccessName(MXL_MONITORING_ACCESS_STARTING)} == "starting");
    CHECK(std::string{mxlMonitoringAccessName(MXL_MONITORING_ACCESS_OPEN)} == "open");
    CHECK(std::string{mxlMonitoringAccessName(MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE)} == "domain_unavailable");
    CHECK(std::string{mxlMonitoringAccessName(MXL_MONITORING_ACCESS_FLOW_NOT_FOUND)} == "flow_not_found");
    CHECK(std::string{mxlMonitoringAccessName(MXL_MONITORING_ACCESS_FLOW_INVALID)} == "flow_not_found");
    CHECK(std::string{mxlMonitoringAccessName(MXL_MONITORING_ACCESS_PERMISSION_DENIED)} == "permission_denied");
    CHECK(std::string{mxlMonitoringAccessName(MXL_MONITORING_ACCESS_OTHER)} == "flow_error");
    CHECK(std::string{mxlMonitoringAccessName(MXL_MONITORING_ACCESS_DISCONNECTED)} == "disconnected");
}

TEST_CASE("Reopening a flow that still delivers nothing is the same stall", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open(); // stall after 100 ms
    auto const index = context.dueIndex();
    context.now += 150 * Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index, 1);
    REQUIRE(context.snapshot().stalls == 1);
    mxlMonitoringRecordRead(flow, MXL_ERR_FLOW_INVALID, index, 1);
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
    context.now += 150 * Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index + 10, 1);
    CHECK(context.snapshot().stalls == 1);

    INFO("data moving ends it; the next stretch without data is a new stall");
    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, index + 10, 1);
    context.now += 150 * Ms;
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_EARLY, index + 11, 1);
    CHECK(context.snapshot().stalls == 2);
}

TEST_CASE("Disconnecting is not an attempt to reach a flow", "[recording]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_DISCONNECTED, MXL_STATUS_OK);
    auto const s = context.snapshot();
    CHECK(s.access == MXL_MONITORING_ACCESS_OPEN);
    CHECK(s.accessFailures[MXL_MONITORING_ACCESS_DISCONNECTED] == 0);
}

TEST_CASE("The last failure's text is there without a snapshot, and layouts carry version 2", "[recording]")
{
    auto context = Context{};
    auto const flow = context.add();
    REQUIRE(mxlMonitoringDescribe(flow, MXL_MONITORING_CONDITION_ACCESS, MXL_MONITORING_ACCESS_OTHER, MXL_MONITORING_CONTENT_COUNT,
                "cannot open flow x: status 1") == MXL_STATUS_OK);
    char text[MXL_MONITORING_TEXT];
    REQUIRE(mxlMonitoringGetLastErrorDetail(flow, text, sizeof(text)) == MXL_STATUS_OK);
    CHECK(std::string{text} == "cannot open flow x: status 1");
    CHECK(context.snapshot().version == 2);
}
