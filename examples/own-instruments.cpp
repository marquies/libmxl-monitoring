// A reader whose process exports through opentelemetry-cpp of its own and
// not through the library's OTLP exporter: its observable instruments read
// snapshots in their callbacks, and a subscriber hands the events to its
// logger. Part of docs/tutorial.md.
//
//   own-instruments <domain> <flow id>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/mxl.h>
#include <mxl/time.h>
#include <mxl-monitoring/monitoring.h>
#include <mxl-monitoring/reader.h>
#include <opentelemetry/exporters/otlp/otlp_http_log_record_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_log_record_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/sdk/logs/batch_log_record_processor_factory.h>
#include <opentelemetry/sdk/logs/batch_log_record_processor_options.h>
#include <opentelemetry/sdk/logs/logger_provider.h>
#include <opentelemetry/sdk/logs/logger_provider_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>

namespace
{
    namespace otlp = opentelemetry::exporter::otlp;
    namespace sdkmetrics = opentelemetry::sdk::metrics;
    namespace sdklogs = opentelemetry::sdk::logs;
    using opentelemetry::metrics::ObserverResult;
    using opentelemetry::metrics::ObserverResultT;
    using opentelemetry::nostd::get;
    using opentelemetry::nostd::shared_ptr;

    volatile std::sig_atomic_t stopping = 0;

    void onSignal(int)
    {
        stopping = 1;
    }

    std::vector<mxlMonitoringFlowSnapshot> snapshots(mxlMonitoring monitoring)
    {
        auto count = std::size_t{0};
        mxlMonitoringSnapshot(monitoring, nullptr, &count);
        auto all = std::vector<mxlMonitoringFlowSnapshot>(count);
        mxlMonitoringSnapshot(monitoring, all.data(), &count);
        all.resize(std::min(count, all.size()));
        return all;
    }

    // The names and attributes are those of docs/data-model.md, so
    // dashboards built on the library's exporter read these too.
    using Attributes = std::map<std::string, std::string>;

    Attributes flowAttributes(mxlMonitoringFlowSnapshot const& s)
    {
        auto attributes = Attributes{{"mxl.flow.id", s.flowId}};
        // Known once the flow is open.
        switch (s.format)
        {
            case MXL_DATA_FORMAT_VIDEO: attributes["mxl.flow.media_type"] = "video"; break;
            case MXL_DATA_FORMAT_AUDIO: attributes["mxl.flow.media_type"] = "audio"; break;
            case MXL_DATA_FORMAT_DATA:  attributes["mxl.flow.media_type"] = "data"; break;
            case MXL_DATA_FORMAT_EVENT: attributes["mxl.flow.media_type"] = "event"; break;
            default:                    break;
        }
        switch (s.unit)
        {
            case MXL_MONITORING_UNIT_GRAIN:  attributes["mxl.flow.unit"] = "grain"; break;
            case MXL_MONITORING_UNIT_SAMPLE: attributes["mxl.flow.unit"] = "sample"; break;
            default:                         break;
        }
        return attributes;
    }

    void observeReads(ObserverResult result, void* monitoring)
    {
        auto& observer = *get<shared_ptr<ObserverResultT<int64_t>>>(result);
        for (auto const& s : snapshots(static_cast<mxlMonitoring>(monitoring)))
        {
            auto attributes = flowAttributes(s);
            for (auto read = 0; read < MXL_MONITORING_READ_COUNT; ++read)
            {
                if (read != MXL_MONITORING_READ_INVALID_CONTENT)
                {
                    attributes["mxl.read.result"] = mxlMonitoringReadName(static_cast<mxlMonitoringRead>(read));
                    observer.Observe(static_cast<int64_t>(s.reads[read]), attributes);
                }
            }
            // invalid_content is counted per issue, as the library's exporter does.
            attributes["mxl.read.result"] = "invalid_content";
            for (auto issue = 0; issue < MXL_MONITORING_CONTENT_COUNT; ++issue)
            {
                attributes["mxl.content.issue"] = mxlMonitoringContentName(static_cast<mxlMonitoringContent>(issue));
                observer.Observe(static_cast<int64_t>(s.content[issue]), attributes);
            }
        }
    }

    void observeDelivering(ObserverResult result, void* monitoring)
    {
        auto& observer = *get<shared_ptr<ObserverResultT<double>>>(result);
        for (auto const& s : snapshots(static_cast<mxlMonitoring>(monitoring)))
        {
            observer.Observe(s.delivering ? 1.0 : 0.0, flowAttributes(s));
        }
    }

    // On the polling thread: a record handed to the logger's batch
    // processor, nothing that calls back into the context.
    void onEvent(mxlMonitoringEvent const* event, void* logger)
    {
        static char const* const phases[] = {"started", "persisting", "recovered"};
        auto const* const outcome = event->kind == MXL_MONITORING_CONDITION_ACCESS ? mxlMonitoringAccessName(event->access)
                                  : event->kind == MXL_MONITORING_CONDITION_READ   ? mxlMonitoringReadName(event->read)
                                                                                   : mxlMonitoringWriteName(event->write);
        auto record = static_cast<opentelemetry::logs::Logger*>(logger)->CreateLogRecord();
        record->SetSeverity(event->phase == MXL_MONITORING_PHASE_RECOVERED ? opentelemetry::logs::Severity::kInfo
                                                                           : opentelemetry::logs::Severity::kWarn);
        auto body = std::string{outcome} + " " + phases[event->phase];
        if (auto const* issue = mxlMonitoringContentName(event->content))
        {
            body.append(" ").append(issue);
        }
        if (event->detail[0] != '\0')
        {
            body.append(": ").append(event->detail);
        }
        record->SetBody(body);
        record->SetAttribute("mxl.flow.id", event->flowId);
        record->SetAttribute("mxl.event.phase", phases[event->phase]);
        static_cast<opentelemetry::logs::Logger*>(logger)->EmitLogRecord(std::move(record));
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <domain> <flow id>\n", argv[0]);
        return 2;
    }
    char const* domain = argv[1];
    char const* flowId = argv[2];
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    mxlMonitoring monitoring;
    mxlMonitoringFlow flow;
    mxlMonitoringCreate(nullptr, &monitoring);
    mxlMonitoringAddFlow(monitoring, flowId, MXL_MONITORING_ROLE_READER, &flow);

    // The process's own providers, each with an OTLP exporter configured
    // from OTEL_*.
    auto meters = sdkmetrics::MeterProviderFactory::Create();
    meters->AddMetricReader(sdkmetrics::PeriodicExportingMetricReaderFactory::Create(
        otlp::OtlpHttpMetricExporterFactory::Create(otlp::OtlpHttpMetricExporterOptions{}), sdkmetrics::PeriodicExportingMetricReaderOptions{}));
    auto loggers = sdklogs::LoggerProviderFactory::Create(sdklogs::BatchLogRecordProcessorFactory::Create(
        otlp::OtlpHttpLogRecordExporterFactory::Create(otlp::OtlpHttpLogRecordExporterOptions{}), sdklogs::BatchLogRecordProcessorOptions{}));

    auto meter = meters->GetMeter("own-instruments");
    auto reads = meter->CreateInt64ObservableCounter("mxl.reader.reads", "Reads by outcome.", "1");
    reads->AddCallback(observeReads, monitoring);
    auto delivering = meter->CreateDoubleObservableGauge("mxl.reader.delivering", "1 while reads land within a second.", "{flow}");
    delivering->AddCallback(observeDelivering, monitoring);
    auto logger = loggers->GetLogger("own-instruments", "own-instruments");
    mxlMonitoringSubscribe(monitoring, onEvent, logger.get());

    mxlInstance instance = mxlMonitoringCreateInstance(flow, domain, "");
    mxlFlowReader reader;
    if (instance == nullptr || mxlMonitoringCreateFlowReader(flow, instance, "", &reader) != MXL_STATUS_OK)
    {
        std::fprintf(stderr, "cannot open flow %s in %s\n", flowId, domain);
        return 1;
    }
    mxlFlowInfo info;
    mxlFlowReaderGetInfo(reader, &info);
    mxlRational const rate = info.config.common.grainRate;

    // The read loop of hello-world.c.
    auto index = mxlGetCurrentIndex(&rate);
    while (!stopping)
    {
        mxlGrainInfo grain;
        uint8_t* payload;
        auto const status = mxlMonitoringFlowReaderGetGrain(flow, reader, index, 200000000, &grain, &payload);
        if (status == MXL_STATUS_OK)
        {
            ++index;
        }
        else if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
        {
            auto const head = mxlGetCurrentIndex(&rate);
            mxlMonitoringRecordSkipped(flow, head - index);
            index = head;
        }
        else if (status != MXL_ERR_OUT_OF_RANGE_TOO_EARLY && status != MXL_ERR_TIMEOUT)
        {
            break;
        }
        mxlMonitoringPoll(monitoring);
    }

    mxlReleaseFlowReader(instance, reader);
    mxlDestroyInstance(instance);

    // The callbacks and the subscriber read the context: stop them first.
    meters->Shutdown();
    mxlMonitoringUnsubscribe(monitoring, onEvent, logger.get());
    loggers->Shutdown();
    mxlMonitoringDestroy(monitoring);
    return 0;
}
