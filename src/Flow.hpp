#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "mxl-monitoring/content.h"
#include "mxl-monitoring/monitoring.h"

namespace mxl::monitoring
{
    constexpr std::uint64_t NsPerSecond = 1'000'000'000ULL;

    using Counter = std::atomic<std::uint64_t>;

    struct Histogram
    {
        std::array<Counter, MXL_MONITORING_LATENCY_BUCKETS> buckets{};
        Counter count{0};
        Counter sumNs{0};

        void observe(std::int64_t latencyNs) noexcept;
        void copyTo(mxlMonitoringHistogram& out) const noexcept;
    };

    // What one poll knows of one condition, kept between polls. Touched by
    // the polling thread only.
    struct Condition
    {
        bool active = false;
        std::uint64_t seen = 0;     // the count at the previous poll
        std::uint64_t pending = 0;  // occurrences not yet in an event
        std::uint64_t total = 0;
        std::uint64_t startedAt = 0;
        std::uint64_t lastEventAt = 0;
        std::uint64_t lastOccurrenceAt = 0;
        std::uint64_t okAtLastOccurrence = 0;
    };

    struct Monitoring;

    struct Flow
    {
        Flow(Monitoring& owner, std::string id, mxlMonitoringRole role);

        Monitoring& owner;
        std::string const id;
        mxlMonitoringRole const role;

        std::atomic<int> access{MXL_MONITORING_ACCESS_STARTING};
        std::atomic<int> unit{MXL_MONITORING_UNIT_UNKNOWN};
        std::atomic<int> format{MXL_DATA_FORMAT_UNSPECIFIED};
        std::atomic<std::int64_t> rateNumerator{0};
        std::atomic<std::int64_t> rateDenominator{0};
        std::atomic<std::uint64_t> graceNs{0};
        std::atomic<std::uint64_t> stallTimeoutNs{0};
        // Set by mxlMonitoringSetFlowTiming; otherwise the wrappers derive the
        // timing again on every open, from the flow as it is then.
        std::atomic<bool> timingSet{false};

        std::array<Counter, MXL_MONITORING_ACCESS_COUNT> accessFailures{};
        // Failures plus losses into each state: what access conditions follow.
        std::array<Counter, MXL_MONITORING_ACCESS_COUNT> accessOccurrences{};
        std::array<std::atomic<int>, MXL_MONITORING_ACCESS_COUNT> accessStatus{};
        Counter opens{0};
        Counter losses{0};

        std::array<Counter, MXL_MONITORING_READ_COUNT> reads{};
        std::array<Counter, MXL_MONITORING_WRITE_COUNT> writes{};
        std::array<Counter, MXL_MONITORING_CONTENT_COUNT> content{};
        Counter skipped{0};
        Counter stalls{0};

        // Per outcome, the last index and status, for events.
        std::array<Counter, MXL_MONITORING_READ_COUNT> readIndex{};
        std::array<std::atomic<int>, MXL_MONITORING_READ_COUNT> readStatus{};
        std::array<Counter, MXL_MONITORING_WRITE_COUNT> writeIndex{};
        std::array<std::atomic<int>, MXL_MONITORING_WRITE_COUNT> writeStatus{};
        std::array<Counter, MXL_MONITORING_CONTENT_COUNT> contentIndex{};

        std::atomic<int> lastErrorStatus{MXL_STATUS_OK};
        Counter lastErrorIndex{0};
        Counter lastErrorTimeNs{0};

        Histogram arrivalLatency;
        Histogram commitLatency;
        Histogram inputLatency;

        Counter lastIndex{0};
        Counter lastDataTimeNs{0};
        std::atomic<std::int64_t> ageNs{0};
        Counter lastProgressNs{0};

        // Recording-thread state for counting TOO_EARLY and stalls once.
        Counter earlyCountedIndex{std::numeric_limits<std::uint64_t>::max()};
        std::atomic<bool> stallCounted{false};

        // Descriptive state, set when the flow is opened or an error is
        // described: never on the path of a good read or write.
        mutable std::mutex mutex;
        std::string mediaType;
        std::string domain;
        // The libmxl flow the handle reads or writes; "" when disconnected.
        std::string target;
        std::string lastErrorDetail;
        // What each condition's events say, so an event never carries the
        // text of another failure.
        std::array<std::string, MXL_MONITORING_ACCESS_COUNT> accessDetail;
        std::string failureDetail; // read or write errors
        std::array<std::string, MXL_MONITORING_CONTENT_COUNT> contentDetail;
        std::string formatMismatch;
        mxlMonitoringDeclared declared{};
        bool definitionSet = false;
        mxlMonitoringExpectation expected{};
        std::string expectedMediaType;
        bool expectationSet = false;
        // Mismatch bits, copied out of the mutex for the hot path.
        std::atomic<std::uint32_t> mismatch{0};
        std::atomic<bool> checkLegal{false};

        // Polling-thread state.
        std::array<Condition, MXL_MONITORING_ACCESS_COUNT> accessConditions{};
        std::array<Condition, MXL_MONITORING_READ_COUNT> readConditions{};
        std::array<Condition, MXL_MONITORING_WRITE_COUNT> writeConditions{};
        std::array<Condition, MXL_MONITORING_CONTENT_COUNT> contentConditions{};

        mxlRational rate() const noexcept;
        std::uint64_t now() const noexcept;
        // Clock minus the media timestamp of index; the rate must be known.
        std::int64_t latency(std::uint64_t index, std::uint64_t at) const noexcept;
        void error(mxlStatus status, std::uint64_t index, std::uint64_t at) noexcept;
        // Data moved: progress, age, and arrival latency for readers or
        // commit latency for writers.
        void data(std::uint64_t index, std::uint64_t at) noexcept;
        void arrivalOrCommit(std::int64_t latencyNs) noexcept;
        void lose(mxlMonitoringAccess into, mxlStatus status) noexcept;
        // Whether data moved within DeliveringWindowNs of at, while open.
        bool delivering(std::uint64_t at) const noexcept;
        void describeAccess(mxlMonitoringAccess access, std::string detail);
        void describeFailure(std::string detail);
        void describeContent(mxlMonitoringContent issue, std::string detail);
        // Forgets what was known of the flow before a retarget. Called with
        // mutex held.
        void forget();
        std::string targetId() const;
        void recomputeMismatch();
    };

    struct Subscriber
    {
        mxlMonitoringEventFn fn;
        void* user;
    };

    struct Monitoring
    {
        explicit Monitoring(mxlMonitoringConfig const& config);

        std::uint64_t persistIntervalNs;
        std::uint64_t recoverDelayNs;
        std::uint64_t (*clock)(void*);
        void* clockUser;

        // Guards the list of flows, not the flows.
        mutable std::mutex flowsMutex;
        std::vector<std::unique_ptr<Flow>> flows;

        std::mutex pollMutex;
        std::vector<Subscriber> subscribers;

        std::uint64_t now() const noexcept;
    };

    Flow* toFlow(mxlMonitoringFlow flow) noexcept;
    mxlMonitoringFlow toHandle(Flow* flow) noexcept;
    Monitoring* toMonitoring(mxlMonitoring monitoring) noexcept;

    // Copies text into a fixed buffer, NUL-terminated and truncated to fit.
    void copyText(std::string_view text, char* buffer, std::size_t size) noexcept;

    // Statuses with which libmxl says the flow went away under an open reader
    // or writer: the writer restarted, or the flow was deleted.
    bool isLoss(mxlStatus status) noexcept;

    // Whether the domain's files for a flow deny this process access.
    bool permissionDenied(std::string const& domain, std::string const& flowId);

    // Delivering means data moved within the last second.
    constexpr std::uint64_t DeliveringWindowNs = NsPerSecond;

    // a - b, or 0 when a clock stepped back and b is later than a.
    constexpr std::uint64_t since(std::uint64_t a, std::uint64_t b) noexcept
    {
        return a >= b ? a - b : 0;
    }
}
