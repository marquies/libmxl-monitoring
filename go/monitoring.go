// Package mxlmonitoring binds the C API of libmxl-monitoring: flows,
// recording, snapshots and the Prometheus text.
//
// libmxl itself is left to go-mxl, so a process loads the one libmxl.so that
// go-mxl links. The static libraries are found through CGO_LDFLAGS, e.g.
// -L<build>, where <build> holds libmxl-monitoring.a and
// libmxl-monitoring-prometheus.a.
//
// The headers in include/mxl-monitoring/ are the reference for what each call
// counts; the names here follow them.
package mxlmonitoring

/*
#cgo CFLAGS: -I${SRCDIR}/../include
#cgo pkg-config: libmxl
#cgo LDFLAGS: -lmxl-monitoring-prometheus -lmxl-monitoring -lstdc++ -lm
#include <stdlib.h>
#include <mxl-monitoring/monitoring.h>
#include <mxl-monitoring/prometheus.h>
*/
import "C"

import (
	"errors"
	"fmt"
	"time"
	"unsafe"

	"github.com/qvest-digital/go-mxl/mxl"
)

// Role tells a media function's flows apart from a gateway's.
type Role int32

// Roles.
const (
	RoleReader            Role = C.MXL_MONITORING_ROLE_READER
	RoleWriter            Role = C.MXL_MONITORING_ROLE_WRITER
	RoleReplicationSource Role = C.MXL_MONITORING_ROLE_REPLICATION_SOURCE
	RoleReplicationTarget Role = C.MXL_MONITORING_ROLE_REPLICATION_TARGET
)

// Unit is what a flow's indices and counts are in.
type Unit int32

// Units.
const (
	UnitUnknown Unit = C.MXL_MONITORING_UNIT_UNKNOWN
	UnitGrain   Unit = C.MXL_MONITORING_UNIT_GRAIN
	UnitSample  Unit = C.MXL_MONITORING_UNIT_SAMPLE
)

// Access is whether a flow can be reached, and if not, why.
type Access int32

// Access states.
const (
	AccessStarting          Access = C.MXL_MONITORING_ACCESS_STARTING
	AccessOpen              Access = C.MXL_MONITORING_ACCESS_OPEN
	AccessDomainUnavailable Access = C.MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE
	AccessFlowNotFound      Access = C.MXL_MONITORING_ACCESS_FLOW_NOT_FOUND
	AccessPermissionDenied  Access = C.MXL_MONITORING_ACCESS_PERMISSION_DENIED
	AccessFlowInvalid       Access = C.MXL_MONITORING_ACCESS_FLOW_INVALID
	AccessOther             Access = C.MXL_MONITORING_ACCESS_OTHER
	AccessDisconnected      Access = C.MXL_MONITORING_ACCESS_DISCONNECTED
	AccessCount                    = C.MXL_MONITORING_ACCESS_COUNT
)

func (a Access) String() string {
	return C.GoString(C.mxlMonitoringAccessName(C.mxlMonitoringAccess(a)))
}

// Read is the outcome of a read.
type Read int32

// Read outcomes.
const (
	ReadOK               Read = C.MXL_MONITORING_READ_OK
	ReadTooEarly         Read = C.MXL_MONITORING_READ_TOO_EARLY
	ReadTooLate          Read = C.MXL_MONITORING_READ_TOO_LATE
	ReadPermissionDenied Read = C.MXL_MONITORING_READ_PERMISSION_DENIED
	ReadError            Read = C.MXL_MONITORING_READ_ERROR
	ReadInvalidContent   Read = C.MXL_MONITORING_READ_INVALID_CONTENT
	ReadCount                 = C.MXL_MONITORING_READ_COUNT
)

func (r Read) String() string {
	return C.GoString(C.mxlMonitoringReadName(C.mxlMonitoringRead(r)))
}

// Write is the outcome of a write.
type Write int32

// Write outcomes.
const (
	WriteOK               Write = C.MXL_MONITORING_WRITE_OK
	WriteInputLate        Write = C.MXL_MONITORING_WRITE_INPUT_LATE
	WriteInvalidInput     Write = C.MXL_MONITORING_WRITE_INVALID_INPUT
	WritePermissionDenied Write = C.MXL_MONITORING_WRITE_PERMISSION_DENIED
	WriteOutOfSpace       Write = C.MXL_MONITORING_WRITE_OUT_OF_SPACE
	WriteError            Write = C.MXL_MONITORING_WRITE_ERROR
	WriteCount                  = C.MXL_MONITORING_WRITE_COUNT
)

func (w Write) String() string {
	return C.GoString(C.mxlMonitoringWriteName(C.mxlMonitoringWrite(w)))
}

// Content is how data read, or input given to a writer, is wrong.
type Content int32

// Content issues.
const (
	ContentWrongFormat     Content = C.MXL_MONITORING_CONTENT_WRONG_FORMAT
	ContentWrongResolution Content = C.MXL_MONITORING_CONTENT_WRONG_RESOLUTION
	ContentWrongRate       Content = C.MXL_MONITORING_CONTENT_WRONG_RATE
	ContentWrongSize       Content = C.MXL_MONITORING_CONTENT_WRONG_SIZE
	ContentMissing         Content = C.MXL_MONITORING_CONTENT_MISSING
	ContentInvalid         Content = C.MXL_MONITORING_CONTENT_INVALID
	ContentIncomplete      Content = C.MXL_MONITORING_CONTENT_INCOMPLETE
	ContentIllegalValues   Content = C.MXL_MONITORING_CONTENT_ILLEGAL_VALUES
	ContentCount                   = C.MXL_MONITORING_CONTENT_COUNT
)

func (c Content) String() string {
	return C.GoString(C.mxlMonitoringContentName(C.mxlMonitoringContent(c)))
}

// status turns what a go-mxl call returned into the libmxl status it carries.
// An error go-mxl did not get from libmxl is MXL_ERR_UNKNOWN.
func status(err error) C.mxlStatus {
	if err == nil {
		return C.MXL_STATUS_OK
	}
	var s mxl.Status
	if errors.As(err, &s) {
		return C.mxlStatus(s)
	}
	return C.MXL_ERR_UNKNOWN
}

func check(op string, s C.mxlStatus) error {
	if s == C.MXL_STATUS_OK {
		return nil
	}
	return fmt.Errorf("%s: %w", op, mxl.Status(s))
}

// ClassifyAccess, ClassifyRead and ClassifyWrite say what a go-mxl error
// means for an open, a read or a write.
func ClassifyAccess(err error) Access {
	return Access(C.mxlMonitoringClassifyAccess(status(err)))
}

// ClassifyRead: see ClassifyAccess.
func ClassifyRead(err error) Read { return Read(C.mxlMonitoringClassifyRead(status(err))) }

// ClassifyWrite: see ClassifyAccess.
func ClassifyWrite(err error) Write { return Write(C.mxlMonitoringClassifyWrite(status(err))) }

// Monitoring is one process's context. Close frees it and every flow in it.
type Monitoring struct {
	c C.mxlMonitoring
}

// New creates a context with the default configuration.
func New() (*Monitoring, error) {
	var m Monitoring
	if err := check("create monitoring", C.mxlMonitoringCreate(nil, &m.c)); err != nil {
		return nil, err
	}
	return &m, nil
}

// Close frees the context. No flow of it may be used afterwards.
func (m *Monitoring) Close() error {
	if m.c == nil {
		return nil
	}
	err := check("destroy monitoring", C.mxlMonitoringDestroy(m.c))
	m.c = nil
	return err
}

// Pointer is the C mxlMonitoring, for a package binding another part of the
// C API, such as an exporter.
func (m *Monitoring) Pointer() unsafe.Pointer { return unsafe.Pointer(m.c) }

// Poll advances every condition to now and delivers its events. Call from one
// goroutine, more often than once a second.
func (m *Monitoring) Poll() error {
	return check("poll", C.mxlMonitoringPoll(m.c))
}

// Flow is a handle for one flow the process reads or writes. One goroutine
// records for a given flow at a time. A nil *Flow records nothing, so a
// process whose monitoring failed keeps its media path running.
type Flow struct {
	c C.mxlMonitoringFlow
}

// AddFlow adds a flow reported as id. The id is also the libmxl flow it
// opens unless SetTarget names another.
func (m *Monitoring) AddFlow(id string, role Role) (*Flow, error) {
	cid := C.CString(id)
	defer C.free(unsafe.Pointer(cid))
	var f Flow
	if err := check("add flow", C.mxlMonitoringAddFlow(m.c, cid, C.mxlMonitoringRole(role), &f.c)); err != nil {
		return nil, err
	}
	return &f, nil
}

// RemoveFlow removes the flow. Nothing may record on it during or after.
func (m *Monitoring) RemoveFlow(f *Flow) error {
	return check("remove flow", C.mxlMonitoringRemoveFlow(m.c, f.c))
}

// SetTarget points the handle at another libmxl flow; "" disconnects it.
func (f *Flow) SetTarget(flowID string) error {
	if f == nil {
		return nil
	}
	cid := C.CString(flowID)
	defer C.free(unsafe.Pointer(cid))
	return check("set flow target", C.mxlMonitoringSetFlowTarget(f.c, cid))
}

// Access is where the flow stands now; AccessStarting for a nil handle.
func (f *Flow) Access() Access {
	if f == nil {
		return AccessStarting
	}
	return Access(C.mxlMonitoringGetFlowAccess(f.c))
}

// SetFormat says what the flow is, once known. mediaType is the flow
// definition's media_type and may be "".
func (f *Flow) SetFormat(format mxl.DataFormat, unit Unit, rate mxl.Rational, mediaType string) error {
	if f == nil {
		return nil
	}
	ff := C.mxlMonitoringFlowFormat{
		version: 1,
		size:    C.uint32_t(unsafe.Sizeof(C.mxlMonitoringFlowFormat{})),
		format:  C.mxlDataFormat(format),
		unit:    C.mxlMonitoringUnit(unit),
		rate:    C.mxlRational{numerator: C.int64_t(rate.Num), denominator: C.int64_t(rate.Den)},
	}
	if mediaType != "" {
		ff.mediaType = C.CString(mediaType)
		defer C.free(unsafe.Pointer(ff.mediaType))
	}
	return check("set flow format", C.mxlMonitoringSetFlowFormat(f.c, &ff))
}

// SetTiming sets when a too-early read and a lack of data become faults.
func (f *Flow) SetTiming(grace, stallTimeout time.Duration) error {
	if f == nil {
		return nil
	}
	return check("set flow timing",
		C.mxlMonitoringSetFlowTiming(f.c, C.uint64_t(grace.Nanoseconds()), C.uint64_t(stallTimeout.Nanoseconds())))
}

// RecordAccess records an attempt to open a reader or writer on the flow:
// the error of the go-mxl call, nil for success. As in the reader wrappers,
// replaced data fails an open as AccessOther; it is a loss only under a
// reader or writer already open.
func (f *Flow) RecordAccess(err error) {
	access := AccessOpen
	if err != nil {
		if access = ClassifyAccess(err); access == AccessFlowInvalid {
			access = AccessOther
		}
	}
	f.RecordAccessAs(access, err)
}

// RecordAccessAs records an attempt to reach the flow whose outcome no libmxl
// status names, such as AccessDomainUnavailable for an instance go-mxl could
// not create.
func (f *Flow) RecordAccessAs(access Access, err error) {
	if f == nil {
		return
	}
	C.mxlMonitoringRecordAccess(f.c, C.mxlMonitoringAccess(access), status(err))
}

// RecordRead records one read: its error, the index read and the grains or
// samples a successful read covers.
func (f *Flow) RecordRead(err error, index, count uint64) {
	if f == nil {
		return
	}
	C.mxlMonitoringRecordRead(f.c, status(err), C.uint64_t(index), C.uint64_t(count))
}

// RecordReadContent records data read that turned out wrong, in place of
// RecordRead for that read.
func (f *Flow) RecordReadContent(issue Content, index, count uint64) {
	if f == nil {
		return
	}
	C.mxlMonitoringRecordReadContent(f.c, C.mxlMonitoringContent(issue), C.uint64_t(index), C.uint64_t(count))
}

// RecordSkipped records grains or samples jumped over after a too-late read.
func (f *Flow) RecordSkipped(count uint64) {
	if f == nil {
		return
	}
	C.mxlMonitoringRecordSkipped(f.c, C.uint64_t(count))
}

// RecordCommit records when index was committed, for a reader: the flow's
// LastWriteTime while index is the head.
func (f *Flow) RecordCommit(index, commitTimeNs uint64) {
	if f == nil {
		return
	}
	C.mxlMonitoringRecordCommit(f.c, C.uint64_t(index), C.uint64_t(commitTimeNs))
}

// RecordWrite records one commit, or a failure to open for writing.
func (f *Flow) RecordWrite(err error, index, count uint64) {
	if f == nil {
		return
	}
	C.mxlMonitoringRecordWrite(f.c, status(err), C.uint64_t(index), C.uint64_t(count))
}

// RecordWriteOutcome records WriteInputLate or WriteOutOfSpace, which no
// libmxl status carries.
func (f *Flow) RecordWriteOutcome(outcome Write, err error, index, count uint64) {
	if f == nil {
		return
	}
	C.mxlMonitoringRecordWriteOutcome(f.c, C.mxlMonitoringWrite(outcome), status(err), C.uint64_t(index), C.uint64_t(count))
}

// RecordWriteContent records input found wrong before writing.
func (f *Flow) RecordWriteContent(issue Content, index, count uint64) {
	if f == nil {
		return
	}
	C.mxlMonitoringRecordWriteContent(f.c, C.mxlMonitoringContent(issue), C.uint64_t(index), C.uint64_t(count))
}

// RecordInput records when the input for index arrived.
func (f *Flow) RecordInput(index, arrivalNs uint64) {
	if f == nil {
		return
	}
	C.mxlMonitoringRecordInput(f.c, C.uint64_t(index), C.uint64_t(arrivalNs))
}

// LatencyBuckets is how many buckets a Histogram has; LatencyBound gives
// their upper bounds.
const LatencyBuckets = C.MXL_MONITORING_LATENCY_BUCKETS

// LatencyBound is the upper bound of bucket i; the last bucket has none.
func LatencyBound(i int) time.Duration {
	return time.Duration(C.mxlMonitoringLatencyBound(C.size_t(i)))
}

// Histogram is a latency distribution. Buckets are per bucket, not
// cumulative.
type Histogram struct {
	Buckets [LatencyBuckets]uint64
	Count   uint64
	Sum     time.Duration
}

func histogram(c *C.mxlMonitoringHistogram) Histogram {
	h := Histogram{Count: uint64(c.count), Sum: time.Duration(c.sumNs)}
	for i := range h.Buckets {
		h.Buckets[i] = uint64(c.buckets[i])
	}
	return h
}

// Snapshot is a copy of one flow's counts. Count arrays are indexed by
// Access, Read, Write and Content.
type Snapshot struct {
	FlowID         string
	Target         string
	Role           Role
	Unit           Unit
	Format         mxl.DataFormat
	Rate           mxl.Rational
	MediaType      string
	Access         Access
	AccessFailures [AccessCount]uint64
	Opens          uint64
	Losses         uint64
	Reads          [ReadCount]uint64
	Writes         [WriteCount]uint64
	Content        [ContentCount]uint64
	Skipped        uint64
	Stalls         uint64
	LastError      error
	LastErrorIndex uint64
	LastErrorTime  uint64
	LastErrorText  string
	FormatMismatch string
	ArrivalLatency Histogram
	CommitLatency  Histogram
	InputLatency   Histogram
	LastIndex      uint64
	LastDataTime   uint64
	Age            time.Duration
	Delivering     bool
}

// Snapshot copies every flow's counts, in the order the flows were added.
func (m *Monitoring) Snapshot() ([]Snapshot, error) {
	var count C.size_t
	if err := check("snapshot", C.mxlMonitoringSnapshot(m.c, nil, &count)); err != nil {
		return nil, err
	}
	cs := make([]C.mxlMonitoringFlowSnapshot, count)
	n := count
	var p *C.mxlMonitoringFlowSnapshot
	if n > 0 {
		p = &cs[0]
	}
	if err := check("snapshot", C.mxlMonitoringSnapshot(m.c, p, &n)); err != nil {
		return nil, err
	}
	// Flows added between the two calls are left for the next snapshot.
	out := make([]Snapshot, min(n, count))
	for i := range out {
		c := &cs[i]
		s := &out[i]
		s.FlowID = C.GoString(&c.flowId[0])
		s.Target = C.GoString(&c.target[0])
		s.Role = Role(c.role)
		s.Unit = Unit(c.unit)
		s.Format = mxl.DataFormat(c.format)
		s.Rate = mxl.Rational{Num: int64(c.rate.numerator), Den: int64(c.rate.denominator)}
		s.MediaType = C.GoString(&c.mediaType[0])
		s.Access = Access(c.access)
		for j := range s.AccessFailures {
			s.AccessFailures[j] = uint64(c.accessFailures[j])
		}
		s.Opens = uint64(c.opens)
		s.Losses = uint64(c.losses)
		for j := range s.Reads {
			s.Reads[j] = uint64(c.reads[j])
		}
		for j := range s.Writes {
			s.Writes[j] = uint64(c.writes[j])
		}
		for j := range s.Content {
			s.Content[j] = uint64(c.content[j])
		}
		s.Skipped = uint64(c.skipped)
		s.Stalls = uint64(c.stalls)
		if c.lastErrorStatus != C.MXL_STATUS_OK {
			s.LastError = mxl.Status(c.lastErrorStatus)
		}
		s.LastErrorIndex = uint64(c.lastErrorIndex)
		s.LastErrorTime = uint64(c.lastErrorTimeNs)
		s.LastErrorText = C.GoString(&c.lastErrorDetail[0])
		s.FormatMismatch = C.GoString(&c.formatMismatch[0])
		s.ArrivalLatency = histogram(&c.arrivalLatency)
		s.CommitLatency = histogram(&c.commitLatency)
		s.InputLatency = histogram(&c.inputLatency)
		s.LastIndex = uint64(c.lastIndex)
		s.LastDataTime = uint64(c.lastDataTimeNs)
		s.Age = time.Duration(c.ageNs)
		s.Delivering = bool(c.delivering)
	}
	return out, nil
}

// PrometheusText renders every flow as Prometheus text exposition.
func (m *Monitoring) PrometheusText() (string, error) {
	var size C.size_t
	// The first call only sizes the buffer; it reports a NULL buffer as
	// INVALID_ARG by design.
	C.mxlMonitoringPrometheusRender(m.c, nil, &size)
	for {
		buf := (*C.char)(C.malloc(size))
		need := size
		s := C.mxlMonitoringPrometheusRender(m.c, buf, &need)
		if s == C.MXL_STATUS_OK {
			text := C.GoString(buf)
			C.free(unsafe.Pointer(buf))
			return text, nil
		}
		C.free(unsafe.Pointer(buf))
		// A flow added between the calls needs a larger buffer.
		if s != C.MXL_ERR_INVALID_ARG || need <= size {
			return "", check("render prometheus text", s)
		}
		size = need
	}
}
