// The OTLP exporter against a collector stand-in in the test process: what
// arrives is what a collector would get, as OTLP JSON over HTTP, so the names,
// the attributes and the sequence of events are checked as they go out.

#include <cstdlib>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>
#include <picojson/picojson.h>
#include "mxl-monitoring/otlp.h"
#include "Support.hpp"

using namespace mxl::monitoring::tests;

namespace
{
    // Answers every POST with 200 and keeps the path and the body.
    class Collector
    {
    public:
        Collector()
        {
            _listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            auto const one = 1;
            ::setsockopt(_listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            auto addr = sockaddr_in{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            REQUIRE(::bind(_listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            REQUIRE(::listen(_listener, 16) == 0);
            auto length = socklen_t{sizeof(addr)};
            ::getsockname(_listener, reinterpret_cast<sockaddr*>(&addr), &length);
            port = ntohs(addr.sin_port);
            _thread = std::thread{[this]
                {
                    serve();
                }};
        }

        ~Collector()
        {
            _stop = true;
            _thread.join();
            ::close(_listener);
        }

        Collector(Collector const&) = delete;
        Collector& operator=(Collector const&) = delete;

        // Every body received on path, parsed.
        std::vector<picojson::value> received(std::string const& path)
        {
            auto const lock = std::lock_guard{_mutex};
            auto out = std::vector<picojson::value>{};
            for (auto const& [p, body] : _requests)
            {
                auto json = picojson::value{};
                if (p == path && picojson::parse(json, body).empty())
                {
                    out.push_back(json);
                }
            }
            return out;
        }

        std::uint16_t port = 0;

    private:
        void serve()
        {
            while (!_stop)
            {
                auto pfd = pollfd{.fd = _listener, .events = POLLIN, .revents = 0};
                if (::poll(&pfd, 1, 50) != 1)
                {
                    continue;
                }
                auto const fd = ::accept4(_listener, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd >= 0)
                {
                    handle(fd);
                    ::close(fd);
                }
            }
        }

        void handle(int fd)
        {
            auto in = std::string{};
            char buffer[65536];
            auto headerEnd = std::string::npos;
            auto continued = false;
            while (true)
            {
                if (headerEnd == std::string::npos && (headerEnd = in.find("\r\n\r\n")) != std::string::npos && !continued &&
                    in.find("100-continue") < headerEnd)
                {
                    constexpr char Continue[] = "HTTP/1.1 100 Continue\r\n\r\n";
                    ::send(fd, Continue, sizeof(Continue) - 1, MSG_NOSIGNAL);
                    continued = true;
                }
                if (headerEnd != std::string::npos)
                {
                    auto const at = in.find("Content-Length:");
                    auto const length = at == std::string::npos ? 0 : std::stoul(in.substr(at + 15));
                    if (in.size() >= headerEnd + 4 + length)
                    {
                        auto const path = in.substr(in.find(' ') + 1, in.find(' ', in.find(' ') + 1) - in.find(' ') - 1);
                        {
                            auto const lock = std::lock_guard{_mutex};
                            _requests.emplace_back(path, in.substr(headerEnd + 4, length));
                        }
                        constexpr char Ok[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
                        ::send(fd, Ok, sizeof(Ok) - 1, MSG_NOSIGNAL);
                        return;
                    }
                }
                auto pfd = pollfd{.fd = fd, .events = POLLIN, .revents = 0};
                if (::poll(&pfd, 1, 2000) != 1)
                {
                    return;
                }
                auto const n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n <= 0)
                {
                    return;
                }
                in.append(buffer, static_cast<std::size_t>(n));
            }
        }

        int _listener = -1;
        std::atomic<bool> _stop{false};
        std::thread _thread;
        std::mutex _mutex;
        std::vector<std::pair<std::string, std::string>> _requests;
    };

    // The OTLP environment for an exporter sending to collector.
    void configure(Collector const& collector)
    {
        ::setenv("OTEL_EXPORTER_OTLP_ENDPOINT", ("http://127.0.0.1:" + std::to_string(collector.port)).c_str(), 1);
        ::setenv("OTEL_EXPORTER_OTLP_PROTOCOL", "http/json", 1);
        // Exported on stop; no export in between makes the counts exact.
        ::setenv("OTEL_METRIC_EXPORT_INTERVAL", "600000", 1);
        ::unsetenv("OTEL_SERVICE_NAME");
    }

    picojson::value const& at(picojson::value const& v, char const* key)
    {
        static auto const null = picojson::value{};
        if (!v.is<picojson::object>())
        {
            return null;
        }
        auto const& o = v.get<picojson::object>();
        auto const it = o.find(key);
        return it == o.end() ? null : it->second;
    }

    picojson::array const& list(picojson::value const& v, char const* key)
    {
        static auto const empty = picojson::array{};
        auto const& a = at(v, key);
        return a.is<picojson::array>() ? a.get<picojson::array>() : empty;
    }

    // An OTLP AnyValue as text: int64 travels as a JSON string.
    std::string text(picojson::value const& anyValue)
    {
        for (auto const* key : {"stringValue", "intValue", "doubleValue", "boolValue"})
        {
            if (auto const& v = at(anyValue, key); !v.is<picojson::null>())
            {
                return v.is<std::string>() ? v.get<std::string>() : v.serialize();
            }
        }
        return {};
    }

    // A plain JSON field as text: a number, or an int64 OTLP sends as a string.
    std::string field(picojson::value const& v, char const* key)
    {
        auto const& f = at(v, key);
        return f.is<std::string>() ? f.get<std::string>() : f.is<picojson::null>() ? std::string{} : f.serialize();
    }

    std::map<std::string, std::string> attributes(picojson::value const& item)
    {
        auto out = std::map<std::string, std::string>{};
        for (auto const& kv : list(item, "attributes"))
        {
            out[at(kv, "key").get<std::string>()] = text(at(kv, "value"));
        }
        return out;
    }

    struct Metric
    {
        picojson::value json;
        // Of the sum, gauge or histogram.
        picojson::value data;
    };

    std::map<std::string, Metric> metrics(Collector& collector)
    {
        auto out = std::map<std::string, Metric>{};
        auto const bodies = collector.received("/v1/metrics");
        REQUIRE_FALSE(bodies.empty());
        // The last export holds the final counts.
        for (auto const& rm : list(bodies.back(), "resourceMetrics"))
        {
            for (auto const& sm : list(rm, "scopeMetrics"))
            {
                for (auto const& m : list(sm, "metrics"))
                {
                    for (auto const* kind : {"sum", "gauge", "histogram"})
                    {
                        if (!at(m, kind).is<picojson::null>())
                        {
                            out[at(m, "name").get<std::string>()] = {m, at(m, kind)};
                        }
                    }
                }
            }
        }
        return out;
    }

    // The point of metric whose attributes include every one given.
    std::optional<picojson::value> point(Metric const& metric, std::map<std::string, std::string> const& wanted)
    {
        for (auto const& p : list(metric.data, "dataPoints"))
        {
            auto const have = attributes(p);
            auto match = true;
            for (auto const& [k, v] : wanted)
            {
                auto const it = have.find(k);
                match = match && it != have.end() && it->second == v;
            }
            if (match)
            {
                return p;
            }
        }
        return std::nullopt;
    }

    std::string intValue(picojson::value const& p)
    {
        return text(p.get<picojson::object>().contains("asInt") ? picojson::value{picojson::object{{"intValue", at(p, "asInt")}}}
                                                                : picojson::value{picojson::object{{"doubleValue", at(p, "asDouble")}}});
    }

    std::vector<picojson::value> logRecords(Collector& collector)
    {
        auto out = std::vector<picojson::value>{};
        for (auto const& body : collector.received("/v1/logs"))
        {
            for (auto const& rl : list(body, "resourceLogs"))
            {
                for (auto const& sl : list(rl, "scopeLogs"))
                {
                    for (auto const& r : list(sl, "logRecords"))
                    {
                        out.push_back(r);
                    }
                }
            }
        }
        return out;
    }

    void deliver(Context& context, mxlMonitoringFlow flow, int grains)
    {
        for (int i = 0; i < grains; ++i)
        {
            context.now += 20 * Ms;
            mxlMonitoringRecordRead(flow, MXL_STATUS_OK, context.dueIndex(), 1);
        }
    }
}

TEST_CASE("Flows are exported as OTLP metrics under their OTLP names", "[otlp]")
{
    auto collector = Collector{};
    configure(collector);
    auto context = Context{};
    auto const reader = context.open();
    deliver(context, reader, 10);
    mxlMonitoringRecordRead(reader, MXL_ERR_OUT_OF_RANGE_TOO_LATE, context.dueIndex(), 1);
    mxlMonitoringRecordRead(reader, MXL_ERR_OUT_OF_RANGE_TOO_LATE, context.dueIndex(), 1);
    mxlMonitoringRecordReadContent(reader, MXL_MONITORING_CONTENT_INVALID, context.dueIndex(), 1);
    auto const writerId = "d4d00000-0000-0000-0000-000000000002";
    auto const writer = context.open(MXL_MONITORING_ROLE_WRITER, writerId);
    mxlMonitoringRecordWrite(writer, MXL_STATUS_OK, context.dueIndex(), 1);
    mxlMonitoringRecordWriteOutcome(writer, MXL_MONITORING_WRITE_INPUT_LATE, MXL_STATUS_OK, context.dueIndex(), 3);

    auto otlp = mxlMonitoringOtlp{};
    REQUIRE(mxlMonitoringOtlpStart(context.monitoring, "mxl-test", &otlp) == MXL_STATUS_OK);
    mxlMonitoringOtlpStop(otlp);

    auto const bodies = collector.received("/v1/metrics");
    REQUIRE(bodies.size() == 1);
    auto const resource = attributes(at(list(bodies[0], "resourceMetrics").at(0), "resource"));
    CHECK(resource.at("service.name") == "mxl-test");

    auto const m = metrics(collector);
    auto const flow = std::map<std::string, std::string>{
        {"mxl.flow.id",         FlowId },
        {"mxl.flow.media_type", "video"},
        {"mxl.flow.unit",       "grain"}
    };
    auto withAttribute = [&](char const* key, char const* value)
    {
        auto a = flow;
        a[key] = value;
        return a;
    };

    REQUIRE(m.contains("mxl.reader.reads"));
    auto const& reads = m.at("mxl.reader.reads");
    INFO("counters are cumulative and monotonic, as the counts behind them");
    CHECK(field(reads.data, "aggregationTemporality") == "2");
    CHECK(at(reads.data, "isMonotonic").evaluate_as_boolean());
    auto const ok = point(reads, withAttribute("mxl.read.result", "ok"));
    REQUIRE(ok);
    CHECK(intValue(*ok) == "10");
    auto const tooLate = point(reads, withAttribute("mxl.read.result", "too_late"));
    REQUIRE(tooLate);
    CHECK(intValue(*tooLate) == "2");
    INFO("invalid content is counted per issue, never as a total beside them");
    auto invalid = withAttribute("mxl.read.result", "invalid_content");
    invalid["mxl.content.issue"] = "invalid";
    auto const invalidPoint = point(reads, invalid);
    REQUIRE(invalidPoint);
    CHECK(intValue(*invalidPoint) == "1");
    for (auto const& p : list(reads.data, "dataPoints"))
    {
        auto const a = attributes(p);
        CHECK((a.at("mxl.read.result") != "invalid_content" || a.contains("mxl.content.issue")));
    }

    auto const state = point(m.at("mxl.flow.state"), withAttribute("mxl.flow.state", "open"));
    REQUIRE(state);
    CHECK(intValue(*state) == "1");
    CHECK(point(m.at("mxl.flow.open.failures"), withAttribute("mxl.open.failure", "flow_not_found")));
    CHECK(point(m.at("mxl.reader.delivering"), flow));

    REQUIRE(m.contains("mxl.reader.arrival.latency"));
    auto const arrival = point(m.at("mxl.reader.arrival.latency"), flow);
    REQUIRE(arrival);
    INFO("ten good grains and one with invalid content arrived");
    CHECK(field(*arrival, "count") == "11");
    CHECK(list(*arrival, "explicitBounds").size() == MXL_MONITORING_LATENCY_BUCKETS - 1);
    CHECK(list(*arrival, "bucketCounts").size() == MXL_MONITORING_LATENCY_BUCKETS);
    CHECK(at(m.at("mxl.reader.arrival.latency").json, "unit").get<std::string>() == "s");

    INFO("a writer's outcomes are writes, under its own id");
    REQUIRE(m.contains("mxl.writer.writes"));
    auto const late = point(m.at("mxl.writer.writes"),
        {
            {"mxl.flow.id",      writerId    },
            {"mxl.write.result", "input_late"}
    });
    REQUIRE(late);
    CHECK(intValue(*late) == "3");
    CHECK_FALSE(point(m.at("mxl.writer.writes"), flow));
    CHECK_FALSE(point(reads,
        {
            {"mxl.flow.id", writerId}
    }));
}

TEST_CASE("A too_late burst is exported as the log records of its events", "[otlp]")
{
    auto collector = Collector{};
    configure(collector);
    auto context = Context{};
    auto const flow = context.open();
    auto otlp = mxlMonitoringOtlp{};
    REQUIRE(mxlMonitoringOtlpStart(context.monitoring, "mxl-test", &otlp) == MXL_STATUS_OK);

    deliver(context, flow, 10);
    mxlMonitoringPoll(context.monitoring);
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 70, 1);
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 71, 1);
    mxlMonitoringRecordRead(flow, MXL_ERR_OUT_OF_RANGE_TOO_LATE, 72, 1);
    mxlMonitoringPoll(context.monitoring);
    deliver(context, flow, 60);
    mxlMonitoringPoll(context.monitoring);
    REQUIRE(context.events.size() == 2);
    mxlMonitoringOtlpStop(otlp);

    auto const records = logRecords(collector);
    REQUIRE(records.size() == 2);
    auto const& failed = records[0];
    CHECK(at(failed, "eventName").get<std::string>() == "mxl.reader.read_failed");
    CHECK(field(failed, "severityNumber") == "13");
    CHECK(text(at(failed, "body")) == "flow d4d00000 too_late, 3 occurrence(s)");
    auto const a = attributes(failed);
    CHECK(a.at("mxl.flow.id") == FlowId);
    CHECK(a.at("mxl.flow.media_type") == "video");
    CHECK(a.at("mxl.read.result") == "too_late");
    CHECK(a.at("mxl.status") == "MXL_ERR_OUT_OF_RANGE_TOO_LATE");
    CHECK(a.at("mxl.event.phase") == "started");
    CHECK(a.at("mxl.grain.index") == "72");
    CHECK(a.at("mxl.occurrences") == "3");

    auto const& recovered = records[1];
    CHECK(at(recovered, "eventName").get<std::string>() == "mxl.reader.read_recovered");
    CHECK(field(recovered, "severityNumber") == "9");
    CHECK(attributes(recovered).at("mxl.total") == "3");
}

TEST_CASE("An exporter with nowhere to send starts, stops and records nothing it should not", "[otlp]")
{
    ::setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:1", 1);
    ::setenv("OTEL_METRIC_EXPORT_INTERVAL", "600000", 1);
    auto context = Context{};
    context.open();
    auto otlp = mxlMonitoringOtlp{};
    CHECK(mxlMonitoringOtlpStart(nullptr, "x", &otlp) == MXL_ERR_INVALID_ARG);
    CHECK(mxlMonitoringOtlpStart(context.monitoring, "x", nullptr) == MXL_ERR_INVALID_ARG);
    REQUIRE(mxlMonitoringOtlpStart(context.monitoring, nullptr, &otlp) == MXL_STATUS_OK);
    mxlMonitoringOtlpStop(otlp);
    mxlMonitoringOtlpStop(nullptr);
    INFO("the exporter's subscription ended with it");
    mxlMonitoringRecordRead(context.add(MXL_MONITORING_ROLE_READER, "d4d00000-0000-0000-0000-000000000003"), MXL_ERR_OUT_OF_RANGE_TOO_LATE, 1, 1);
    CHECK(mxlMonitoringPoll(context.monitoring) == MXL_STATUS_OK);
}
