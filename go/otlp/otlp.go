// Package otlp binds the OTLP/HTTP exporter of libmxl-monitoring: every flow's
// metrics and every event as a log record, pushed to an OpenTelemetry
// Collector and configured only through the standard OTEL_* environment
// variables.
//
// It links libmxl-monitoring-otlp.a, built with -DMXL_MONITORING_OTLP=ON, and
// opentelemetry-cpp statically. cgo refuses a compiler flag in
// opentelemetry-cpp's pkg-config files, and those files omit its curl HTTP
// client, so CGO_LDFLAGS carries the libraries instead, grouped with the
// libmxl-monitoring archives so their order does not matter. cgo may place
// the archives this package names after CGO_LDFLAGS, where a group no longer
// resolves them, so the group names them again:
//
//	-Wl,--start-group -lmxl-monitoring-otlp -lmxl-monitoring-prometheus \
//	-lmxl-monitoring -lopentelemetry_http_client_curl \
//	$(pkg-config --static --libs opentelemetry_exporter_otlp_http_metric \
//	    opentelemetry_exporter_otlp_http_log libcurl) -lstdc++ -Wl,--end-group
package otlp

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo pkg-config: libmxl
#cgo LDFLAGS: -lmxl-monitoring-otlp
#include <stdlib.h>
#include <mxl-monitoring/otlp.h>
*/
import "C"

import (
	"fmt"
	"unsafe"

	"github.com/qvest-digital/go-mxl/mxl"

	mxlmonitoring "github.com/marquies/libmxl-monitoring/go"
)

// Exporter exports one monitoring context until Stop.
type Exporter struct {
	c C.mxlMonitoringOtlp
}

// Start exports m. serviceName is service.name where neither
// OTEL_SERVICE_NAME nor OTEL_RESOURCE_ATTRIBUTES sets it, and may be "". A
// collector that cannot be reached is retried and does not fail the start.
// Log records come from the events Poll delivers, so the process polls m.
func Start(m *mxlmonitoring.Monitoring, serviceName string) (*Exporter, error) {
	var name *C.char
	if serviceName != "" {
		name = C.CString(serviceName)
		defer C.free(unsafe.Pointer(name))
	}
	var e Exporter
	s := C.mxlMonitoringOtlpStart(C.mxlMonitoring(m.Pointer()), name, &e.c)
	if s != C.MXL_STATUS_OK {
		return nil, fmt.Errorf("start OTLP exporter: %w", mxl.Status(s))
	}
	return &e, nil
}

// Stop exports once more, sends the log records still queued, and stops. m
// must outlive it.
func (e *Exporter) Stop() {
	C.mxlMonitoringOtlpStop(e.c)
}
