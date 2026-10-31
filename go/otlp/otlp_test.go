package otlp

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	"github.com/qvest-digital/go-mxl/mxl"

	mxlmonitoring "github.com/marquies/libmxl-monitoring/go"
)

// What a process records through the Go binding reaches a collector: the
// exporter reads the same context the binding records into, and Stop sends
// what is still due.
func TestStopDeliversRecordedFlowsToTheCollector(t *testing.T) {
	var mu sync.Mutex
	bodies := map[string]string{}
	collector := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		mu.Lock()
		bodies[r.URL.Path] += string(body)
		mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte("{}"))
	}))
	defer collector.Close()
	t.Setenv("OTEL_EXPORTER_OTLP_ENDPOINT", collector.URL)
	t.Setenv("OTEL_EXPORTER_OTLP_PROTOCOL", "http/json")

	m, err := mxlmonitoring.New()
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = m.Close() }()
	f, err := m.AddFlow("d4d00000-0000-0000-0000-000000000002", mxlmonitoring.RoleReader)
	if err != nil {
		t.Fatal(err)
	}

	e, err := Start(m, "otlp-test")
	if err != nil {
		t.Fatal(err)
	}
	f.RecordAccess(mxl.ErrFlowNotFound)
	if err := m.Poll(); err != nil {
		t.Fatal(err)
	}
	e.Stop()

	mu.Lock()
	defer mu.Unlock()
	for path, want := range map[string]string{
		"/v1/metrics": "mxl.flow.open.failures",
		"/v1/logs":    "d4d00000-0000-0000-0000-000000000002",
	} {
		if !strings.Contains(bodies[path], want) {
			t.Errorf("%s lacks %q: %q", path, want, bodies[path])
		}
	}
	if !strings.Contains(bodies["/v1/metrics"], "otlp-test") {
		t.Errorf("metrics lack the service name: %q", bodies["/v1/metrics"])
	}
}
