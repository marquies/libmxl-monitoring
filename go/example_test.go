package mxlmonitoring_test

import (
	"bufio"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"

	mxlmonitoring "github.com/marquies/libmxl-monitoring/go"
)

// A process with an HTTP server of its own serves the Prometheus text from a
// handler of its own, after a metric of the process.
func ExampleMonitoring_PrometheusText() {
	m, err := mxlmonitoring.New()
	if err != nil {
		fmt.Println(err)
		return
	}
	defer m.Close()
	flow, err := m.AddFlow("5fbec3b1-1b0f-417d-9059-8b94a47197ed", mxlmonitoring.RoleReader)
	if err != nil {
		fmt.Println(err)
		return
	}
	// What a reader records as it opens the flow and reads three grains.
	flow.RecordAccess(nil)
	for index := uint64(100); index < 103; index++ {
		flow.RecordRead(nil, index, 1)
	}

	var requests atomic.Int64
	mux := http.NewServeMux()
	mux.HandleFunc("/metrics", func(w http.ResponseWriter, _ *http.Request) {
		text, err := m.PrometheusText()
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "text/plain; version=0.0.4")
		fmt.Fprintf(w, "# TYPE own_server_requests_total counter\nown_server_requests_total %d\n", requests.Add(1))
		fmt.Fprint(w, text)
	})
	server := httptest.NewServer(mux)
	defer server.Close()

	response, err := http.Get(server.URL + "/metrics")
	if err != nil {
		fmt.Println(err)
		return
	}
	defer response.Body.Close()
	lines := bufio.NewScanner(response.Body)
	for lines.Scan() {
		if line := lines.Text(); strings.HasPrefix(line, "own_server_") || strings.Contains(line, `reads_total{flow="5fbec3b1-1b0f-417d-9059-8b94a47197ed",result="ok"}`) {
			fmt.Println(line)
		}
	}
	// Output:
	// own_server_requests_total 1
	// mxl_reader_reads_total{flow="5fbec3b1-1b0f-417d-9059-8b94a47197ed",result="ok"} 3
}
