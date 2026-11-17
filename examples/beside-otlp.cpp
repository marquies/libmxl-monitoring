// A reader whose process exports through an opentelemetry-cpp MeterProvider
// of its own, with the library's OTLP exporter beside it in the same process.
// Both push to the collector the OTEL_* variables name. Part of
// docs/tutorial.md.
//
//   beside-otlp <domain> <flow id>

#include <csignal>
#include <cstdio>
#include <memory>
#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/mxl.h>
#include <mxl/time.h>
#include <mxl-monitoring/monitoring.h>
#include <mxl-monitoring/otlp.h>
#include <mxl-monitoring/reader.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>

namespace
{
    namespace otlp = opentelemetry::exporter::otlp;
    namespace sdkmetrics = opentelemetry::sdk::metrics;

    volatile std::sig_atomic_t stopping = 0;

    void onSignal(int)
    {
        stopping = 1;
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

    // The process's own metrics, as it set them up before using the library:
    // a MeterProvider made global, with an OTLP exporter configured from
    // OTEL_*.
    std::shared_ptr<sdkmetrics::MeterProvider> provider = sdkmetrics::MeterProviderFactory::Create();
    provider->AddMetricReader(sdkmetrics::PeriodicExportingMetricReaderFactory::Create(
        otlp::OtlpHttpMetricExporterFactory::Create(otlp::OtlpHttpMetricExporterOptions{}), sdkmetrics::PeriodicExportingMetricReaderOptions{}));
    opentelemetry::metrics::Provider::SetMeterProvider(std::shared_ptr<opentelemetry::metrics::MeterProvider>{provider});
    auto const grains = opentelemetry::metrics::Provider::GetMeterProvider()
                            ->GetMeter("beside-otlp")
                            ->CreateUInt64Counter("beside_otlp.grains", "Grains this process handled.", "{grain}");

    // The library's exporter beside it: a metric exporter and a
    // LoggerProvider of its own, read from the same OTEL_* variables.
    mxlMonitoring monitoring;
    mxlMonitoringFlow flow;
    mxlMonitoringCreate(nullptr, &monitoring);
    mxlMonitoringAddFlow(monitoring, flowId, MXL_MONITORING_ROLE_READER, &flow);
    mxlMonitoringOtlp exporter;
    if (mxlMonitoringOtlpStart(monitoring, "beside-otlp", &exporter) != MXL_STATUS_OK)
    {
        std::fprintf(stderr, "cannot start the OTLP exporter\n");
        return 1;
    }

    mxlInstance instance = mxlMonitoringCreateInstance(flow, domain, "");
    mxlFlowReader reader;
    if (instance == nullptr || mxlMonitoringCreateFlowReader(flow, instance, "", &reader) != MXL_STATUS_OK)
    {
        std::fprintf(stderr, "cannot open flow %s in %s\n", flowId, domain);
        mxlMonitoringOtlpStop(exporter);
        return 1;
    }
    mxlFlowInfo info;
    mxlFlowReaderGetInfo(reader, &info);
    mxlRational const rate = info.config.common.grainRate;

    // The read loop of hello-world.c, counting each grain in the process's
    // own counter as well.
    auto index = mxlGetCurrentIndex(&rate);
    while (!stopping)
    {
        mxlGrainInfo grain;
        uint8_t* payload;
        auto const status = mxlMonitoringFlowReaderGetGrain(flow, reader, index, 200000000, &grain, &payload);
        if (status == MXL_STATUS_OK)
        {
            grains->Add(1);
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
    mxlMonitoringOtlpStop(exporter);
    mxlMonitoringDestroy(monitoring);
    provider->Shutdown();
    return 0;
}
