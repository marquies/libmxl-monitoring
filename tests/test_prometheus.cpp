// The exposition a process serves on /metrics. For readers it is the text
// mxl-reader has served, name for name and label for label, so dashboards and
// alerts built on it read the library's output unchanged.

#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "mxl-monitoring/prometheus.h"
#include "Support.hpp"

using namespace mxl::monitoring::tests;

namespace
{
    std::string render(mxlMonitoring monitoring)
    {
        auto size = std::size_t{0};
        REQUIRE(mxlMonitoringPrometheusRender(monitoring, nullptr, &size) == MXL_ERR_INVALID_ARG);
        auto buffer = std::vector<char>(size);
        REQUIRE(mxlMonitoringPrometheusRender(monitoring, buffer.data(), &size) == MXL_STATUS_OK);
        return buffer.data();
    }

    bool contains(std::string const& text, std::string const& line)
    {
        return text.find(line + "\n") != std::string::npos;
    }

    constexpr char const* ReaderText = R"(# HELP mxl_reader_flow_info Media type and unit of a flow, once it has been opened.
# TYPE mxl_reader_flow_info gauge
mxl_reader_flow_info{flow="d4d00000-0000-0000-0000-000000000001",media_type="video/v210",unit="grains"} 1
# HELP mxl_reader_flow_state 1 for the state the flow's reader is in.
# TYPE mxl_reader_flow_state gauge
mxl_reader_flow_state{flow="d4d00000-0000-0000-0000-000000000001",state="open"} 1
# HELP mxl_reader_flow_delivering 1 while reads land within a second.
# TYPE mxl_reader_flow_delivering gauge
mxl_reader_flow_delivering{flow="d4d00000-0000-0000-0000-000000000001"} 1
# HELP mxl_reader_reads_total Reads by outcome, in grains (discrete) or samples (continuous); too_early and too_late count events.
# TYPE mxl_reader_reads_total counter
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="ok"} 1
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="too_early"} 0
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="too_late"} 1
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="missing"} 1
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="invalid"} 0
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="incomplete"} 0
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="wrong_size"} 0
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="wrong_format"} 2
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="illegal_values"} 0
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="permission_error"} 0
mxl_reader_reads_total{flow="d4d00000-0000-0000-0000-000000000001",result="other_error"} 0
# HELP mxl_reader_open_failures_total Attempts to reach a flow that failed, by reason.
# TYPE mxl_reader_open_failures_total counter
mxl_reader_open_failures_total{flow="d4d00000-0000-0000-0000-000000000001",reason="domain"} 0
mxl_reader_open_failures_total{flow="d4d00000-0000-0000-0000-000000000001",reason="not_found"} 0
mxl_reader_open_failures_total{flow="d4d00000-0000-0000-0000-000000000001",reason="permission"} 0
mxl_reader_open_failures_total{flow="d4d00000-0000-0000-0000-000000000001",reason="other"} 1
# HELP mxl_reader_skipped_total Grains or samples jumped over to reach the head again after the ring overtook the reader.
# TYPE mxl_reader_skipped_total counter
mxl_reader_skipped_total{flow="d4d00000-0000-0000-0000-000000000001"} 4
# HELP mxl_reader_stalls_total Read timeouts that passed with no read at all.
# TYPE mxl_reader_stalls_total counter
mxl_reader_stalls_total{flow="d4d00000-0000-0000-0000-000000000001"} 0
# HELP mxl_reader_opens_total Times the flow was opened.
# TYPE mxl_reader_opens_total counter
mxl_reader_opens_total{flow="d4d00000-0000-0000-0000-000000000001"} 2
# HELP mxl_reader_lost_total Times an open flow went away and was reopened.
# TYPE mxl_reader_lost_total counter
mxl_reader_lost_total{flow="d4d00000-0000-0000-0000-000000000001"} 0
# HELP mxl_reader_age_seconds Wall clock minus the media timestamp of the last read.
# TYPE mxl_reader_age_seconds gauge
mxl_reader_age_seconds{flow="d4d00000-0000-0000-0000-000000000001"} 0.003000
)";
}

TEST_CASE("A reader's exposition is mxl-reader's, followed by its latencies", "[prometheus]")
{
    auto context = Context{};
    auto const flow = context.open();
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_FLOW_INVALID, MXL_ERR_FLOW_INVALID);
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
    auto const index = context.dueIndex();
    context.now += 3 * Ms;
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_MISSING, index - 1, 1);
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_WRONG_RATE, index - 1, 1);
    mxlMonitoringRecordReadContent(flow, MXL_MONITORING_CONTENT_WRONG_RESOLUTION, index - 1, 1);
    mxlMonitoringRecordRead(flow, MXL_STATUS_OK, index, 1);
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, index + 1, 1);
    mxlMonitoringRecordSkipped(flow, 4);

    auto const text = render(context.monitoring);
    INFO(text);
    REQUIRE(text.starts_with(ReaderText));

    auto const id = std::string{FlowId};
    CHECK(contains(text, "# TYPE mxl_reader_arrival_seconds histogram"));
    CHECK(contains(text, "mxl_reader_arrival_seconds_bucket{flow=\"" + id + "\",le=\"0.002\"} 0"));
    CHECK(contains(text, "mxl_reader_arrival_seconds_bucket{flow=\"" + id + "\",le=\"0.005\"} 1"));
    CHECK(contains(text, "mxl_reader_arrival_seconds_bucket{flow=\"" + id + "\",le=\"0.04\"} 4"));
    CHECK(contains(text, "mxl_reader_arrival_seconds_bucket{flow=\"" + id + "\",le=\"+Inf\"} 4"));
    CHECK(contains(text, "mxl_reader_arrival_seconds_sum{flow=\"" + id + "\"} 0.072000"));
    CHECK(contains(text, "mxl_reader_arrival_seconds_count{flow=\"" + id + "\"} 4"));
    CHECK(contains(text, "mxl_reader_commit_seconds_count{flow=\"" + id + "\"} 0"));
    CHECK(text.find("mxl_writer") == std::string::npos);
}

TEST_CASE("A flow not yet opened has a state but no info or age", "[prometheus]")
{
    auto context = Context{};
    auto const flow = context.add();
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_FLOW_NOT_FOUND, MXL_ERR_FLOW_NOT_FOUND);
    auto const text = render(context.monitoring);
    auto const id = std::string{FlowId};
    CHECK(text.find("mxl_reader_flow_info{") == std::string::npos);
    CHECK(text.find("mxl_reader_age_seconds{") == std::string::npos);
    CHECK(contains(text, "mxl_reader_flow_state{flow=\"" + id + "\",state=\"flow_not_found\"} 1"));
    CHECK(contains(text, "mxl_reader_open_failures_total{flow=\"" + id + "\",reason=\"not_found\"} 1"));
}

TEST_CASE("A writer's exposition counts writes, by outcome and by input issue", "[prometheus]")
{
    auto context = Context{};
    auto const flow = context.open(MXL_MONITORING_ROLE_WRITER);
    mxlMonitoringRecordWrite(flow, MXL_STATUS_OK, context.dueIndex(), 1);
    mxlMonitoringRecordWriteOutcome(flow, MXL_MONITORING_WRITE_INPUT_LATE, MXL_STATUS_OK, 1, 3);
    mxlMonitoringRecordWriteContent(flow, MXL_MONITORING_CONTENT_WRONG_RATE, 2, 1);

    auto const text = render(context.monitoring);
    auto const id = std::string{FlowId};
    CHECK(contains(text, "mxl_writer_writes_total{flow=\"" + id + "\",result=\"ok\"} 1"));
    CHECK(contains(text, "mxl_writer_writes_total{flow=\"" + id + "\",result=\"input_late\"} 3"));
    CHECK(contains(text, "mxl_writer_writes_total{flow=\"" + id + "\",result=\"invalid_input\"} 1"));
    CHECK(contains(text, "mxl_writer_invalid_input_total{flow=\"" + id + "\",issue=\"wrong_rate\"} 1"));
    CHECK(contains(text, "mxl_writer_commit_seconds_count{flow=\"" + id + "\"} 1"));
    CHECK(text.find("mxl_writer_reads_total") == std::string::npos);
    CHECK(text.find("mxl_writer_stalls_total") == std::string::npos);
    CHECK(text.find("mxl_reader") == std::string::npos);
}

TEST_CASE("Replication flows carry the gateway prefixes", "[prometheus]")
{
    auto context = Context{};
    context.open(MXL_MONITORING_ROLE_REPLICATION_SOURCE);
    context.open(MXL_MONITORING_ROLE_REPLICATION_TARGET);
    auto const text = render(context.monitoring);
    CHECK(text.find("mxl_gateway_source_reads_total{") != std::string::npos);
    CHECK(text.find("mxl_gateway_target_writes_total{") != std::string::npos);
}

TEST_CASE("A flow id or media type cannot break a label", "[prometheus]")
{
    auto context = Context{};
    auto const flow = context.add(MXL_MONITORING_ROLE_READER, "a\"b\\c\nd");
    auto format = mxlMonitoringFlowFormat{};
    format.version = 1;
    format.size = sizeof(format);
    format.mediaType = "video/\"x\"";
    REQUIRE(mxlMonitoringSetFlowFormat(flow, &format) == MXL_STATUS_OK);
    mxlMonitoringRecordAccess(flow, MXL_MONITORING_ACCESS_OPEN, MXL_STATUS_OK);
    auto const text = render(context.monitoring);
    CHECK(contains(text, "mxl_reader_flow_info{flow=\"a_b_c_d\",media_type=\"video/_x_\",unit=\"grains\"} 1"));
}

TEST_CASE("A buffer too small is refused with the size it needs", "[prometheus]")
{
    auto context = Context{};
    context.open();
    auto size = std::size_t{0};
    REQUIRE(mxlMonitoringPrometheusRender(context.monitoring, nullptr, &size) == MXL_ERR_INVALID_ARG);
    auto buffer = std::vector<char>(size - 1);
    auto small = buffer.size();
    CHECK(mxlMonitoringPrometheusRender(context.monitoring, buffer.data(), &small) == MXL_ERR_INVALID_ARG);
    CHECK(small == size);
}

TEST_CASE("A slot shows its name, a disconnected state, and no info without a media type", "[prometheus]")
{
    auto context = Context{};
    auto const flow = context.open(MXL_MONITORING_ROLE_READER, "camera-1");
    REQUIRE(mxlMonitoringSetFlowTarget(flow, "") == MXL_STATUS_OK);
    auto const text = render(context.monitoring);
    CHECK(contains(text, "mxl_reader_flow_state{flow=\"camera-1\",state=\"disconnected\"} 1"));
    INFO("opened before, but nothing is known of a flow now");
    CHECK(text.find("mxl_reader_flow_info{") == std::string::npos);
    CHECK(contains(text, "mxl_reader_opens_total{flow=\"camera-1\"} 1"));
}
