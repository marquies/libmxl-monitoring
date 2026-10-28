#include "mxl-monitoring/otlp.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <opentelemetry/exporters/otlp/otlp_http_log_record_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_log_record_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/sdk/instrumentationscope/instrumentation_scope.h>
#include <opentelemetry/sdk/logs/batch_log_record_processor_factory.h>
#include <opentelemetry/sdk/logs/batch_log_record_processor_options.h>
#include <opentelemetry/sdk/logs/logger_provider.h>
#include <opentelemetry/sdk/logs/logger_provider_factory.h>
#include <opentelemetry/sdk/metrics/export/metric_producer.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/push_metric_exporter.h>
#include <opentelemetry/sdk/resource/resource.h>

namespace
{
    namespace metrics = opentelemetry::sdk::metrics;
    namespace resource = opentelemetry::sdk::resource;
    namespace logs = opentelemetry::logs;
    using opentelemetry::common::SystemTimestamp;

    SystemTimestamp wallClock()
    {
        return SystemTimestamp{std::chrono::system_clock::now()};
    }

    char const* statusName(mxlStatus status)
    {
        switch (status)
        {
            case MXL_STATUS_OK:                  return "MXL_STATUS_OK";
            case MXL_ERR_UNKNOWN:                return "MXL_ERR_UNKNOWN";
            case MXL_ERR_FLOW_NOT_FOUND:         return "MXL_ERR_FLOW_NOT_FOUND";
            case MXL_ERR_OUT_OF_RANGE_TOO_LATE:  return "MXL_ERR_OUT_OF_RANGE_TOO_LATE";
            case MXL_ERR_OUT_OF_RANGE_TOO_EARLY: return "MXL_ERR_OUT_OF_RANGE_TOO_EARLY";
            case MXL_ERR_INVALID_FLOW_READER:    return "MXL_ERR_INVALID_FLOW_READER";
            case MXL_ERR_INVALID_FLOW_WRITER:    return "MXL_ERR_INVALID_FLOW_WRITER";
            case MXL_ERR_TIMEOUT:                return "MXL_ERR_TIMEOUT";
            case MXL_ERR_INVALID_ARG:            return "MXL_ERR_INVALID_ARG";
            case MXL_ERR_CONFLICT:               return "MXL_ERR_CONFLICT";
            case MXL_ERR_PERMISSION_DENIED:      return "MXL_ERR_PERMISSION_DENIED";
            case MXL_ERR_FLOW_INVALID:           return "MXL_ERR_FLOW_INVALID";
            case MXL_ERR_STRLEN:                 return "MXL_ERR_STRLEN";
            case MXL_ERR_INTERRUPTED:            return "MXL_ERR_INTERRUPTED";
            case MXL_ERR_NO_FABRIC:              return "MXL_ERR_NO_FABRIC";
            case MXL_ERR_INVALID_STATE:          return "MXL_ERR_INVALID_STATE";
            case MXL_ERR_INTERNAL:               return "MXL_ERR_INTERNAL";
            case MXL_ERR_NOT_READY:              return "MXL_ERR_NOT_READY";
            case MXL_ERR_NOT_FOUND:              return "MXL_ERR_NOT_FOUND";
            case MXL_ERR_EXISTS:                 return "MXL_ERR_EXISTS";
            case MXL_ERR_UNSUPPORTED_OPERATION:  return "MXL_ERR_UNSUPPORTED_OPERATION";
        }
        return "MXL_ERR_UNKNOWN";
    }

    // mxl.flow.media_type is the kind of flow, not its definition's media
    // type, so a dashboard can group by it.
    char const* formatName(mxlDataFormat format)
    {
        switch (format)
        {
            case MXL_DATA_FORMAT_VIDEO: return "video";
            case MXL_DATA_FORMAT_AUDIO: return "audio";
            case MXL_DATA_FORMAT_DATA:  return "data";
            case MXL_DATA_FORMAT_EVENT: return "event";
            default:                    return nullptr;
        }
    }

    char const* unitName(mxlMonitoringUnit unit)
    {
        switch (unit)
        {
            case MXL_MONITORING_UNIT_GRAIN:  return "grain";
            case MXL_MONITORING_UNIT_SAMPLE: return "sample";
            default:                         return nullptr;
        }
    }

    char const* contentName(mxlMonitoringContent content)
    {
        switch (content)
        {
            case MXL_MONITORING_CONTENT_WRONG_FORMAT:     return "wrong_format";
            case MXL_MONITORING_CONTENT_WRONG_RESOLUTION: return "wrong_resolution";
            case MXL_MONITORING_CONTENT_WRONG_RATE:       return "wrong_rate";
            case MXL_MONITORING_CONTENT_WRONG_SIZE:       return "wrong_size";
            case MXL_MONITORING_CONTENT_MISSING:          return "missing";
            case MXL_MONITORING_CONTENT_INVALID:          return "invalid";
            case MXL_MONITORING_CONTENT_INCOMPLETE:       return "incomplete";
            case MXL_MONITORING_CONTENT_ILLEGAL_VALUES:   return "illegal_values";
            default:                                      return nullptr;
        }
    }

    char const* readName(mxlMonitoringRead read)
    {
        switch (read)
        {
            case MXL_MONITORING_READ_OK:                return "ok";
            case MXL_MONITORING_READ_TOO_EARLY:         return "too_early";
            case MXL_MONITORING_READ_TOO_LATE:          return "too_late";
            case MXL_MONITORING_READ_PERMISSION_DENIED: return "permission_denied";
            case MXL_MONITORING_READ_ERROR:             return "read_error";
            case MXL_MONITORING_READ_INVALID_CONTENT:   return "invalid_content";
            default:                                    return "read_error";
        }
    }

    char const* writeName(mxlMonitoringWrite write)
    {
        switch (write)
        {
            case MXL_MONITORING_WRITE_OK:                return "ok";
            case MXL_MONITORING_WRITE_INPUT_LATE:        return "input_late";
            case MXL_MONITORING_WRITE_INVALID_INPUT:     return "invalid_input";
            case MXL_MONITORING_WRITE_PERMISSION_DENIED: return "permission_denied";
            case MXL_MONITORING_WRITE_OUT_OF_SPACE:      return "out_of_space";
            case MXL_MONITORING_WRITE_ERROR:             return "write_error";
            default:                                     return "write_error";
        }
    }

    // mxl.open.failure: the reasons an attempt to reach a flow fails.
    char const* openFailureName(mxlMonitoringAccess access)
    {
        switch (access)
        {
            case MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE: return "domain_unavailable";
            case MXL_MONITORING_ACCESS_FLOW_NOT_FOUND:     return "flow_not_found";
            case MXL_MONITORING_ACCESS_PERMISSION_DENIED:  return "permission_denied";
            case MXL_MONITORING_ACCESS_FLOW_INVALID:       return "flow_invalid";
            default:                                       return "other";
        }
    }

    // The metrics of one export, each with its points in the order added.
    class Batch
    {
    public:
        Batch(SystemTimestamp start, SystemTimestamp end)
            : _start{start}
            , _end{end}
        {}

        void counter(char const* name, char const* description, char const* unit, metrics::PointAttributes attributes, std::uint64_t value)
        {
            auto point = metrics::SumPointData{};
            point.value_ = static_cast<std::int64_t>(value);
            point.is_monotonic_ = true;
            add(name, description, unit, metrics::InstrumentType::kCounter, std::move(attributes), std::move(point));
        }

        void gauge(char const* name, char const* description, char const* unit, metrics::PointAttributes attributes, metrics::ValueType value)
        {
            auto point = metrics::LastValuePointData{};
            point.value_ = value;
            point.is_lastvalue_valid_ = true;
            point.sample_ts_ = _end;
            add(name, description, unit, metrics::InstrumentType::kGauge, std::move(attributes), std::move(point));
        }

        void histogram(char const* name, char const* description, metrics::PointAttributes attributes, mxlMonitoringHistogram const& h)
        {
            auto point = metrics::HistogramPointData{};
            for (std::size_t i = 0; i + 1 < MXL_MONITORING_LATENCY_BUCKETS; ++i)
            {
                point.boundaries_.push_back(static_cast<double>(mxlMonitoringLatencyBound(i)) / 1e9);
            }
            point.counts_.assign(std::begin(h.buckets), std::end(h.buckets));
            point.count_ = h.count;
            point.sum_ = static_cast<double>(h.sumNs) / 1e9;
            point.record_min_max_ = false;
            add(name, description, "s", metrics::InstrumentType::kHistogram, std::move(attributes), std::move(point));
        }

        std::vector<metrics::MetricData> take()
        {
            return std::move(_data);
        }

    private:
        void add(char const* name, char const* description, char const* unit, metrics::InstrumentType type, metrics::PointAttributes attributes,
            metrics::PointType point)
        {
            auto [it, added] = _index.try_emplace(name, _data.size());
            if (added)
            {
                auto data = metrics::MetricData{};
                data.instrument_descriptor = {name,
                    description,
                    unit,
                    type,
                    type == metrics::InstrumentType::kGauge ? metrics::InstrumentValueType::kDouble : metrics::InstrumentValueType::kLong};
                data.aggregation_temporality = metrics::AggregationTemporality::kCumulative;
                data.start_ts = _start;
                data.end_ts = _end;
                _data.push_back(std::move(data));
            }
            _data[it->second].point_data_attr_.push_back({std::move(attributes), std::move(point)});
        }

        SystemTimestamp _start;
        SystemTimestamp _end;
        std::map<std::string, std::size_t> _index;
        std::vector<metrics::MetricData> _data;
    };

    metrics::PointAttributes flowAttributes(mxlMonitoringFlowSnapshot const& s)
    {
        auto attributes = metrics::PointAttributes{};
        attributes.SetAttribute("mxl.flow.id", std::string{s.flowId});
        if (auto const* media = formatName(s.format))
        {
            attributes.SetAttribute("mxl.flow.media_type", media);
        }
        if (auto const* unit = unitName(s.unit))
        {
            attributes.SetAttribute("mxl.flow.unit", unit);
        }
        return attributes;
    }

    metrics::PointAttributes with(metrics::PointAttributes attributes, char const* key, char const* value)
    {
        attributes.SetAttribute(key, value);
        return attributes;
    }

    void addFlow(Batch& batch, mxlMonitoringFlowSnapshot const& s)
    {
        auto const base = flowAttributes(s);
        for (auto access = 0; access < MXL_MONITORING_ACCESS_COUNT; ++access)
        {
            auto const a = static_cast<mxlMonitoringAccess>(access);
            batch.gauge("mxl.flow.state",
                "1 for the state the flow is in.",
                "1",
                with(base, "mxl.flow.state", mxlMonitoringAccessName(a)),
                static_cast<std::int64_t>(s.access == a ? 1 : 0));
        }
        // FLOW_INVALID and OTHER are failures of their own here, unlike the
        // Prometheus reasons, which fold them into other.
        for (auto const a : {MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE,
                 MXL_MONITORING_ACCESS_FLOW_NOT_FOUND,
                 MXL_MONITORING_ACCESS_PERMISSION_DENIED,
                 MXL_MONITORING_ACCESS_FLOW_INVALID,
                 MXL_MONITORING_ACCESS_OTHER})
        {
            batch.counter("mxl.flow.open.failures",
                "Attempts to reach the flow that failed, by reason.",
                "{attempt}",
                with(base, "mxl.open.failure", openFailureName(a)),
                s.accessFailures[a]);
        }
        batch.counter("mxl.flow.opens", "Times the flow was opened.", "{open}", base, s.opens);
        batch.counter("mxl.flow.reopens", "Times an open flow went away.", "{open}", base, s.losses);

        auto const content = [&](char const* name, char const* description, char const* resultKey, char const* result)
        {
            for (auto issue = 0; issue < MXL_MONITORING_CONTENT_COUNT; ++issue)
            {
                auto attributes = with(base, resultKey, result);
                attributes.SetAttribute("mxl.content.issue", contentName(static_cast<mxlMonitoringContent>(issue)));
                batch.counter(name, description, "1", std::move(attributes), s.content[issue]);
            }
        };

        switch (s.role)
        {
            case MXL_MONITORING_ROLE_READER:
            {
                char const* const reads = "Reads by outcome; ok and invalid_content in grains or samples, the rest in occurrences.";
                for (auto read = 0; read < MXL_MONITORING_READ_COUNT; ++read)
                {
                    // Invalid content is counted per issue below.
                    if (read != MXL_MONITORING_READ_INVALID_CONTENT)
                    {
                        batch.counter("mxl.reader.reads",
                            reads,
                            "1",
                            with(base, "mxl.read.result", readName(static_cast<mxlMonitoringRead>(read))),
                            s.reads[read]);
                    }
                }
                content("mxl.reader.reads", reads, "mxl.read.result", "invalid_content");
                batch.counter("mxl.reader.skipped", "Grains or samples jumped over to reach the head again.", "1", base, s.skipped);
                batch.counter("mxl.reader.stalls", "Read timeouts that passed with no read at all.", "{timeout}", base, s.stalls);
                batch.gauge("mxl.reader.delivering", "1 while reads land within a second.", "1", base, static_cast<std::int64_t>(s.delivering));
                batch.histogram("mxl.reader.arrival.latency", "Read time minus the media timestamp of what was read.", base, s.arrivalLatency);
                batch.histogram("mxl.reader.commit.latency", "Commit time minus the media timestamp, for reads of the head.", base, s.commitLatency);
                if (s.lastDataTimeNs != 0)
                {
                    batch.gauge("mxl.reader.age", "Clock minus the media timestamp of the last read.", "s", base, static_cast<double>(s.ageNs) / 1e9);
                }
                break;
            }
            case MXL_MONITORING_ROLE_WRITER:
            {
                char const* const writes =
                    "Writes by outcome; ok, input_late, invalid_input and out_of_space in grains or samples, the rest in occurrences.";
                for (auto write = 0; write < MXL_MONITORING_WRITE_COUNT; ++write)
                {
                    if (write != MXL_MONITORING_WRITE_INVALID_INPUT)
                    {
                        batch.counter("mxl.writer.writes",
                            writes,
                            "1",
                            with(base, "mxl.write.result", writeName(static_cast<mxlMonitoringWrite>(write))),
                            s.writes[write]);
                    }
                }
                content("mxl.writer.writes", writes, "mxl.write.result", "invalid_input");
                batch.gauge("mxl.writer.delivering", "1 while commits land within a second.", "1", base, static_cast<std::int64_t>(s.delivering));
                batch.histogram("mxl.writer.commit.latency", "Commit time minus the media timestamp of what was committed.", base, s.commitLatency);
                batch.histogram("mxl.writer.input.latency", "Input arrival minus the media timestamp it is written at.", base, s.inputLatency);
                if (s.lastDataTimeNs != 0)
                {
                    batch.gauge("mxl.writer.head", "Index of the last grain or sample written.", "1", base, static_cast<std::int64_t>(s.lastIndex));
                }
                break;
            }
            // The gateway's mxl.replication.* instruments come with the
            // gateway.
            default: break;
        }
    }

    // What the events of one condition are called, by role and phase.
    char const* eventName(mxlMonitoringEvent const& e)
    {
        auto const recovered = e.phase == MXL_MONITORING_PHASE_RECOVERED;
        switch (e.kind)
        {
            case MXL_MONITORING_CONDITION_ACCESS:
                return recovered ? "mxl.flow.opened" : e.phase == MXL_MONITORING_PHASE_STARTED ? "mxl.flow.open_failed" : "mxl.flow.open_failing";
            case MXL_MONITORING_CONDITION_READ:  return recovered ? "mxl.reader.read_recovered" : "mxl.reader.read_failed";
            case MXL_MONITORING_CONDITION_WRITE: return recovered ? "mxl.writer.write_recovered" : "mxl.writer.write_failed";
        }
        return "mxl.event";
    }

    logs::Severity severity(mxlMonitoringEvent const& e)
    {
        if (e.phase == MXL_MONITORING_PHASE_RECOVERED)
        {
            return logs::Severity::kInfo;
        }
        auto const timing =
            (e.kind == MXL_MONITORING_CONDITION_READ && (e.read == MXL_MONITORING_READ_TOO_EARLY || e.read == MXL_MONITORING_READ_TOO_LATE)) ||
            (e.kind == MXL_MONITORING_CONDITION_WRITE && e.write == MXL_MONITORING_WRITE_INPUT_LATE);
        return timing ? logs::Severity::kWarn : logs::Severity::kError;
    }

    char const* phaseName(mxlMonitoringPhase phase)
    {
        switch (phase)
        {
            case MXL_MONITORING_PHASE_STARTED:    return "started";
            case MXL_MONITORING_PHASE_PERSISTING: return "persisting";
            case MXL_MONITORING_PHASE_RECOVERED:  return "recovered";
        }
        return "started";
    }

    // One sentence for a person: the core's own text where it has one, which
    // timing conditions do not.
    std::string body(mxlMonitoringEvent const& e)
    {
        auto const id = std::string_view{e.flowId}.substr(0, 8);
        auto const outcome = e.kind == MXL_MONITORING_CONDITION_ACCESS ? openFailureName(e.access)
                           : e.kind == MXL_MONITORING_CONDITION_READ   ? readName(e.read)
                                                                       : writeName(e.write);
        auto const* issue = contentName(e.content);
        auto text = std::format("flow {} {}{}{}", id, outcome, issue != nullptr ? " " : "", issue != nullptr ? issue : "");
        switch (e.phase)
        {
            case MXL_MONITORING_PHASE_STARTED:    text += std::format(", {} occurrence(s)", e.occurrences); break;
            case MXL_MONITORING_PHASE_PERSISTING: text += std::format(" persisting, {} more, {} in all", e.occurrences, e.total); break;
            case MXL_MONITORING_PHASE_RECOVERED:  text += std::format(" over after {} occurrence(s)", e.total); break;
        }
        return e.detail[0] != '\0' ? text + ": " + e.detail : text;
    }

    // service.name from the environment where it is set there, else the
    // caller's, else the SDK's unknown_service.
    resource::Resource makeResource(char const* serviceName)
    {
        auto detected = resource::Resource::Create({});
        auto const& attributes = detected.GetAttributes();
        auto const it = attributes.find("service.name");
        auto const unset = it == attributes.end() || opentelemetry::nostd::get<std::string>(it->second).starts_with("unknown_service");
        if (serviceName == nullptr || *serviceName == '\0' || !unset)
        {
            return detected;
        }
        return resource::Resource::Create({
            {"service.name", std::string{serviceName}}
        });
    }
}

struct mxlMonitoringOtlp_t
{
    mxlMonitoring monitoring = nullptr;
    resource::Resource resource = resource::Resource::GetEmpty();
    opentelemetry::nostd::unique_ptr<opentelemetry::sdk::instrumentationscope::InstrumentationScope> scope;
    std::unique_ptr<metrics::PushMetricExporter> metricExporter;
    std::unique_ptr<opentelemetry::sdk::logs::LoggerProvider> loggerProvider;
    opentelemetry::nostd::shared_ptr<logs::Logger> logger;
    SystemTimestamp start;
    std::chrono::milliseconds interval{60000};

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread thread;

    void exportOnce()
    {
        auto count = std::size_t{0};
        mxlMonitoringSnapshot(monitoring, nullptr, &count);
        auto snapshots = std::vector<mxlMonitoringFlowSnapshot>(count);
        mxlMonitoringSnapshot(monitoring, snapshots.data(), &count);
        snapshots.resize(std::min(count, snapshots.size()));

        auto batch = Batch{start, wallClock()};
        for (auto const& s : snapshots)
        {
            addFlow(batch, s);
        }
        auto scopeMetrics = metrics::ScopeMetrics{};
        scopeMetrics.scope_ = scope.get();
        scopeMetrics.metric_data_ = batch.take();
        auto data = metrics::ResourceMetrics{};
        data.resource_ = &resource;
        data.scope_metric_data_.push_back(std::move(scopeMetrics));
        // A collector that is not reachable is tried again next interval;
        // the counts are cumulative, so nothing is lost but resolution.
        metricExporter->Export(data);
    }

    void run()
    {
        auto lock = std::unique_lock{mutex};
        while (!wake.wait_for(lock, interval, [this] { return stopping; }))
        {
            lock.unlock();
            exportOnce();
            lock.lock();
        }
    }

    void emit(mxlMonitoringEvent const& e)
    {
        auto record = logger->CreateLogRecord();
        if (!record)
        {
            return;
        }
        record->SetTimestamp(wallClock());
        record->SetSeverity(severity(e));
        record->SetEventId(0, eventName(e));
        record->SetBody(body(e));
        record->SetAttribute("mxl.flow.id", e.flowId);
        if (auto const* media = formatName(e.format))
        {
            record->SetAttribute("mxl.flow.media_type", media);
        }
        if (auto const* unit = unitName(e.unit))
        {
            record->SetAttribute("mxl.flow.unit", unit);
        }
        switch (e.kind)
        {
            case MXL_MONITORING_CONDITION_ACCESS: record->SetAttribute("mxl.open.failure", openFailureName(e.access)); break;
            case MXL_MONITORING_CONDITION_READ:   record->SetAttribute("mxl.read.result", readName(e.read)); break;
            case MXL_MONITORING_CONDITION_WRITE:  record->SetAttribute("mxl.write.result", writeName(e.write)); break;
        }
        if (auto const* issue = contentName(e.content))
        {
            record->SetAttribute("mxl.content.issue", issue);
        }
        record->SetAttribute("mxl.event.phase", phaseName(e.phase));
        record->SetAttribute("mxl.status", statusName(e.status));
        record->SetAttribute("mxl.grain.index", static_cast<std::int64_t>(e.index));
        record->SetAttribute("mxl.occurrences", static_cast<std::int64_t>(e.occurrences));
        record->SetAttribute("mxl.total", static_cast<std::int64_t>(e.total));
        record->SetAttribute("mxl.duration", static_cast<double>(e.durationNs) / 1e9);
        logger->EmitLogRecord(std::move(record));
    }

    static void onEvent(mxlMonitoringEvent const* event, void* user)
    {
        static_cast<mxlMonitoringOtlp_t*>(user)->emit(*event);
    }
};

extern "C"
{
    mxlStatus mxlMonitoringOtlpStart(mxlMonitoring monitoring, char const* serviceName, mxlMonitoringOtlp* otlp)
    {
        if (monitoring == nullptr || otlp == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        try
        {
            namespace otlpExporter = opentelemetry::exporter::otlp;
            auto o = std::make_unique<mxlMonitoringOtlp_t>();
            o->monitoring = monitoring;
            o->resource = makeResource(serviceName);
            o->scope = opentelemetry::sdk::instrumentationscope::InstrumentationScope::Create("libmxl-monitoring");
            o->metricExporter = otlpExporter::OtlpHttpMetricExporterFactory::Create(otlpExporter::OtlpHttpMetricExporterOptions{});
            o->loggerProvider = opentelemetry::sdk::logs::LoggerProviderFactory::Create(
                opentelemetry::sdk::logs::BatchLogRecordProcessorFactory::Create(
                    otlpExporter::OtlpHttpLogRecordExporterFactory::Create(otlpExporter::OtlpHttpLogRecordExporterOptions{}),
                    opentelemetry::sdk::logs::BatchLogRecordProcessorOptions{}),
                o->resource);
            o->logger = o->loggerProvider->GetLogger("libmxl-monitoring", "libmxl-monitoring");
            o->interval = metrics::PeriodicExportingMetricReaderOptions{}.export_interval_millis;
            o->start = wallClock();
            o->thread = std::thread{[p = o.get()]
                {
                    p->run();
                }};
            // Last, so nothing that can still fail leaves a subscription to
            // a freed exporter behind.
            if (mxlMonitoringSubscribe(monitoring, &mxlMonitoringOtlp_t::onEvent, o.get()) != MXL_STATUS_OK)
            {
                {
                    auto const lock = std::lock_guard{o->mutex};
                    o->stopping = true;
                }
                o->wake.notify_all();
                o->thread.join();
                return MXL_ERR_UNKNOWN;
            }
            *otlp = o.release();
            return MXL_STATUS_OK;
        }
        catch (...)
        {
            return MXL_ERR_UNKNOWN;
        }
    }

    void mxlMonitoringOtlpStop(mxlMonitoringOtlp otlp)
    {
        if (otlp == nullptr)
        {
            return;
        }
        auto const o = std::unique_ptr<mxlMonitoringOtlp_t>{otlp};
        mxlMonitoringUnsubscribe(o->monitoring, &mxlMonitoringOtlp_t::onEvent, o.get());
        {
            auto const lock = std::lock_guard{o->mutex};
            o->stopping = true;
        }
        o->wake.notify_all();
        o->thread.join();
        o->exportOnce();
        o->metricExporter->Shutdown();
        o->loggerProvider->Shutdown();
    }
}
