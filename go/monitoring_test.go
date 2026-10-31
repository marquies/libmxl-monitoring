package mxlmonitoring

import (
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/qvest-digital/go-mxl/mxl"
)

const flowID = "d4d00000-0000-0000-0000-000000000001"

func newReader(t *testing.T) (*Monitoring, *Flow) {
	t.Helper()
	m, err := New()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = m.Close() })
	f, err := m.AddFlow(flowID, RoleReader)
	if err != nil {
		t.Fatal(err)
	}
	return m, f
}

func snapshot(t *testing.T, m *Monitoring) Snapshot {
	t.Helper()
	s, err := m.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	if len(s) != 1 {
		t.Fatalf("snapshot has %d flows, want 1", len(s))
	}
	return s[0]
}

// go-mxl returns libmxl statuses wrapped in context by its callers, so the
// status has to be found through the chain; a binding that compared the
// error itself would count every wrapped too-late read as a read error.
func TestRecordReadFindsTheStatusOfAWrappedError(t *testing.T) {
	m, f := newReader(t)
	f.RecordAccess(nil)
	if got := f.Access(); got != AccessOpen {
		t.Fatalf("access %v after an open, want open", got)
	}
	f.RecordRead(nil, 10, 1)
	f.RecordRead(fmt.Errorf("get grain: %w", mxl.ErrOutOfRangeLate), 11, 1)
	f.RecordRead(errors.New("not from libmxl"), 12, 1)

	s := snapshot(t, m)
	if s.FlowID != flowID || s.Role != RoleReader {
		t.Errorf("flow %q role %d", s.FlowID, s.Role)
	}
	// A read failing for no timing reason leaves the reader unusable.
	if s.Access != AccessOther || s.Opens != 1 {
		t.Errorf("access %v opens %d, want flow_error after one open and a failed read", s.Access, s.Opens)
	}
	if s.Reads[ReadOK] != 1 || s.Reads[ReadTooLate] != 1 || s.Reads[ReadError] != 1 {
		t.Errorf("reads %v, want one ok, one too_late, one read_error", s.Reads)
	}
	if !errors.Is(s.LastError, mxl.ErrUnknown) || s.LastErrorIndex != 12 {
		t.Errorf("last error %v at %d, want MXL_ERR_UNKNOWN at 12", s.LastError, s.LastErrorIndex)
	}
}

// A failed open says why in the access state, and a flow that went away
// under an open reader is a loss of access rather than a failed read.
func TestAccessFollowsOpensAndLosses(t *testing.T) {
	m, f := newReader(t)
	f.RecordAccess(fmt.Errorf("open MXL reader: %w", mxl.ErrFlowNotFound))
	if got := f.Access(); got != AccessFlowNotFound {
		t.Fatalf("access %v after a failed open, want flow_not_found", got)
	}
	f.RecordAccess(nil)
	f.RecordRead(mxl.ErrFlowInvalid, 20, 1)

	s := snapshot(t, m)
	if s.AccessFailures[AccessFlowNotFound] != 1 {
		t.Errorf("access failures %v, want one flow_not_found", s.AccessFailures)
	}
	if s.Losses != 1 || s.Reads[ReadError] != 0 {
		t.Errorf("losses %d read errors %d, want the invalid flow counted as one loss", s.Losses, s.Reads[ReadError])
	}
}

func TestContentIsCountedInsteadOfOK(t *testing.T) {
	m, f := newReader(t)
	f.RecordAccess(nil)
	f.RecordReadContent(ContentInvalid, 30, 1)

	s := snapshot(t, m)
	if s.Reads[ReadOK] != 0 || s.Reads[ReadInvalidContent] != 1 || s.Content[ContentInvalid] != 1 {
		t.Errorf("reads %v content %v, want the grain counted as invalid, not ok", s.Reads, s.Content)
	}
}

func TestSetFormatAndTarget(t *testing.T) {
	m, f := newReader(t)
	rate := mxl.Rational{Num: 50, Den: 1}
	if err := f.SetFormat(mxl.FormatVideo, UnitGrain, rate, "video/v210"); err != nil {
		t.Fatal(err)
	}
	s := snapshot(t, m)
	if s.Format != mxl.FormatVideo || s.Unit != UnitGrain || s.Rate != rate || s.MediaType != "video/v210" {
		t.Errorf("format %v unit %d rate %v media type %q", s.Format, s.Unit, s.Rate, s.MediaType)
	}

	if err := f.SetTarget(""); err != nil {
		t.Fatal(err)
	}
	if got := f.Access(); got != AccessDisconnected {
		t.Errorf("access %v with no target, want disconnected", got)
	}
}

// The Prometheus text keeps mxl-reader's metric names and label values, which
// dashboards and alerts are built on.
func TestCommitLatencyIsObserved(t *testing.T) {
	m, f := newReader(t)
	rate := mxl.Rational{Num: 50, Den: 1}
	if err := f.SetFormat(mxl.FormatVideo, UnitGrain, rate, ""); err != nil {
		t.Fatal(err)
	}
	// Committed 10 ms after its media timestamp.
	index := mxl.TimestampToIndex(rate, mxl.Now()) - 10
	f.RecordCommit(index, mxl.IndexToTimestamp(rate, index)+uint64(10*time.Millisecond))

	h := snapshot(t, m).CommitLatency
	if h.Count != 1 || h.Sum != 10*time.Millisecond {
		t.Errorf("commit latency count %d sum %v, want one of 10ms", h.Count, h.Sum)
	}
	if LatencyBound(0) != time.Millisecond {
		t.Errorf("first bound %v", LatencyBound(0))
	}
}

func TestPrometheusTextCarriesTheReaderNames(t *testing.T) {
	m, f := newReader(t)
	f.RecordAccess(nil)
	f.RecordRead(nil, 40, 1)

	text, err := m.PrometheusText()
	if err != nil {
		t.Fatal(err)
	}
	want := `mxl_reader_reads_total{flow="` + flowID + `",result="ok"} 1`
	if !strings.Contains(text, want) {
		t.Errorf("text lacks %q:\n%s", want, text)
	}
}

func TestPollAndRemoveFlow(t *testing.T) {
	m, f := newReader(t)
	f.RecordAccess(mxl.ErrPermissionDenied)
	if err := m.Poll(); err != nil {
		t.Fatal(err)
	}
	if err := m.RemoveFlow(f); err != nil {
		t.Fatal(err)
	}
	s, err := m.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	if len(s) != 0 {
		t.Errorf("%d flows after removing the only one", len(s))
	}
}

// An open failing because the flow's data was replaced is not a loss, as
// in the C reader wrappers, and an instance that cannot be created is an
// unavailable domain although go-mxl carries no status for it.
func TestOpenFailuresAreClassifiedAsTheWrappersDo(t *testing.T) {
	_, f := newReader(t)
	f.RecordAccess(mxl.ErrFlowInvalid)
	if got := f.Access(); got != AccessOther {
		t.Errorf("access %v after an open failing on replaced data, want flow_error", got)
	}
	f.RecordAccessAs(AccessDomainUnavailable, errors.New("mxl: mxlCreateInstance returned NULL"))
	if got := f.Access(); got != AccessDomainUnavailable {
		t.Errorf("access %v, want domain_unavailable", got)
	}
}

func TestANilFlowRecordsNothing(t *testing.T) {
	var f *Flow
	f.RecordAccess(nil)
	f.RecordRead(nil, 1, 1)
	if err := f.SetTiming(0, 0); err != nil || f.Access() != AccessStarting {
		t.Errorf("nil flow: %v, %v", err, f.Access())
	}
}

func TestCloseTwice(t *testing.T) {
	m, err := New()
	if err != nil {
		t.Fatal(err)
	}
	if err := m.Close(); err != nil {
		t.Fatal(err)
	}
	if err := m.Close(); err != nil {
		t.Errorf("second close: %v", err)
	}
}

func TestNames(t *testing.T) {
	if got := ReadTooLate.String(); got != "too_late" {
		t.Errorf("ReadTooLate is %q", got)
	}
	if got := ClassifyRead(fmt.Errorf("x: %w", mxl.ErrTimeout)); got != ReadTooEarly {
		t.Errorf("a timed-out read is %v, want too_early", got)
	}
	if got := ClassifyAccess(mxl.ErrPermissionDenied); got != AccessPermissionDenied {
		t.Errorf("a denied open is %v", got)
	}
}
