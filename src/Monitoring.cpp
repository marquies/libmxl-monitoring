#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <mxl/time.h>
#include "Flow.hpp"

namespace mxl::monitoring
{
    namespace
    {
        constexpr std::uint64_t Ms = 1'000'000ULL;
        constexpr std::array<std::uint64_t, MXL_MONITORING_LATENCY_BUCKETS - 1> Bounds = {
            1 * Ms, 2 * Ms, 5 * Ms, 10 * Ms, 20 * Ms, 40 * Ms, 80 * Ms, 160 * Ms, 320 * Ms, 640 * Ms, 1280 * Ms};

        constexpr auto Relaxed = std::memory_order_relaxed;

        void add(Counter& counter, std::uint64_t n) noexcept
        {
            counter.fetch_add(n, Relaxed);
        }

        // Counts an occurrence that a poll turns into an event. Release, after
        // the index and status it carries were stored, so a poll that sees the
        // count sees them too.
        void occur(Counter& counter, std::uint64_t n) noexcept
        {
            counter.fetch_add(n, std::memory_order_release);
        }

        bool inRange(int value, int end) noexcept
        {
            return value >= 0 && value < end;
        }

        // A read or write that fails for a reason other than timing leaves the
        // reader or writer unusable; the flow's access follows until data moves
        // again or it is reopened.
        mxlMonitoringAccess accessAfter(mxlMonitoringRead read) noexcept
        {
            return read == MXL_MONITORING_READ_PERMISSION_DENIED ? MXL_MONITORING_ACCESS_PERMISSION_DENIED : MXL_MONITORING_ACCESS_OTHER;
        }
    }

    void Histogram::observe(std::int64_t latencyNs) noexcept
    {
        auto const ns = static_cast<std::uint64_t>(std::max<std::int64_t>(latencyNs, 0));
        auto const bucket = static_cast<std::size_t>(std::ranges::lower_bound(Bounds, ns) - Bounds.begin());
        add(buckets[bucket], 1);
        add(count, 1);
        add(sumNs, ns);
    }

    void Histogram::copyTo(mxlMonitoringHistogram& out) const noexcept
    {
        for (std::size_t i = 0; i < buckets.size(); ++i)
        {
            out.buckets[i] = buckets[i].load(Relaxed);
        }
        out.count = count.load(Relaxed);
        out.sumNs = sumNs.load(Relaxed);
    }

    Flow::Flow(Monitoring& owner, std::string id, mxlMonitoringRole role)
        : owner{owner}
        , id{std::move(id)}
        , role{role}
    {}

    mxlRational Flow::rate() const noexcept
    {
        return mxlRational{rateNumerator.load(Relaxed), rateDenominator.load(Relaxed)};
    }

    std::uint64_t Flow::now() const noexcept
    {
        return owner.now();
    }

    std::int64_t Flow::latency(std::uint64_t index, std::uint64_t at) const noexcept
    {
        auto const r = rate();
        return static_cast<std::int64_t>(at) - static_cast<std::int64_t>(mxlIndexToTimestamp(&r, index));
    }

    void Flow::error(mxlStatus status, std::uint64_t index, std::uint64_t at) noexcept
    {
        lastErrorStatus.store(status, Relaxed);
        lastErrorIndex.store(index, Relaxed);
        lastErrorTimeNs.store(at, Relaxed);
    }

    void Flow::data(std::uint64_t index, std::uint64_t at) noexcept
    {
        lastIndex.store(index, Relaxed);
        lastDataTimeNs.store(at, Relaxed);
        // Without a rate there is no media timestamp to measure against.
        if (rate().numerator > 0 && rate().denominator > 0)
        {
            auto const ns = latency(index, at);
            ageNs.store(ns, Relaxed);
            arrivalOrCommit(ns);
        }
        lastProgressNs.store(at, Relaxed);
        stallCounted.store(false, Relaxed);
        access.store(MXL_MONITORING_ACCESS_OPEN, Relaxed);
    }

    void Flow::arrivalOrCommit(std::int64_t latencyNs) noexcept
    {
        auto const reads = role == MXL_MONITORING_ROLE_READER || role == MXL_MONITORING_ROLE_REPLICATION_SOURCE;
        (reads ? arrivalLatency : commitLatency).observe(latencyNs);
    }

    // Once per open: a caller retrying the read before reopening has still
    // lost the flow once.
    void Flow::lose(mxlMonitoringAccess into, mxlStatus status) noexcept
    {
        accessStatus[into].store(status, Relaxed);
        error(status, 0, now());
        if (access.exchange(into, Relaxed) == MXL_MONITORING_ACCESS_OPEN)
        {
            add(losses, 1);
            occur(accessOccurrences[into], 1);
        }
    }

    bool Flow::delivering(std::uint64_t at) const noexcept
    {
        auto const last = lastDataTimeNs.load(Relaxed);
        return access.load(Relaxed) == MXL_MONITORING_ACCESS_OPEN && last != 0 && at >= last && at - last < DeliveringWindowNs;
    }

    void Flow::describeAccess(mxlMonitoringAccess which, std::string detail)
    {
        auto const lock = std::lock_guard{mutex};
        lastErrorDetail = detail;
        accessDetail[which] = std::move(detail);
    }

    void Flow::describeFailure(std::string detail)
    {
        auto const lock = std::lock_guard{mutex};
        lastErrorDetail = detail;
        failureDetail = std::move(detail);
    }

    void Flow::describeContent(mxlMonitoringContent issue, std::string detail)
    {
        auto const lock = std::lock_guard{mutex};
        lastErrorDetail = detail;
        contentDetail[issue] = std::move(detail);
    }

    void Flow::forget()
    {
        mediaType.clear();
        lastErrorDetail.clear();
        failureDetail.clear();
        for (auto& d : accessDetail)
        {
            d.clear();
        }
        for (auto& d : contentDetail)
        {
            d.clear();
        }
        declared = mxlMonitoringDeclared{};
        definitionSet = false;
        recomputeMismatch();
        unit.store(MXL_MONITORING_UNIT_UNKNOWN, Relaxed);
        format.store(MXL_DATA_FORMAT_UNSPECIFIED, Relaxed);
        rateNumerator.store(0, Relaxed);
        rateDenominator.store(0, Relaxed);
        lastErrorStatus.store(MXL_STATUS_OK, Relaxed);
        lastErrorIndex.store(0, Relaxed);
        lastErrorTimeNs.store(0, Relaxed);
        lastIndex.store(0, Relaxed);
        lastDataTimeNs.store(0, Relaxed);
        ageNs.store(0, Relaxed);
        lastProgressNs.store(0, Relaxed);
        earlyCountedIndex.store(std::numeric_limits<std::uint64_t>::max(), Relaxed);
        access.store(target.empty() ? MXL_MONITORING_ACCESS_DISCONNECTED : MXL_MONITORING_ACCESS_STARTING, Relaxed);
    }

    std::string Flow::targetId() const
    {
        auto const lock = std::lock_guard{mutex};
        return target;
    }

    bool isLoss(mxlStatus status) noexcept
    {
        switch (status)
        {
            case MXL_ERR_FLOW_NOT_FOUND:
            case MXL_ERR_FLOW_INVALID:
            case MXL_ERR_INVALID_FLOW_READER:
            case MXL_ERR_INVALID_FLOW_WRITER: return true;
            default:                          return false;
        }
    }

    Monitoring::Monitoring(mxlMonitoringConfig const& config)
        : persistIntervalNs{config.persistIntervalNs != 0 ? config.persistIntervalNs : 60 * NsPerSecond}
        , recoverDelayNs{config.recoverDelayNs != 0 ? config.recoverDelayNs : NsPerSecond}
        , clock{config.now}
        , clockUser{config.nowUser}
    {}

    std::uint64_t Monitoring::now() const noexcept
    {
        return clock != nullptr ? clock(clockUser) : mxlGetTime();
    }

    Flow* toFlow(mxlMonitoringFlow flow) noexcept
    {
        return reinterpret_cast<Flow*>(flow);
    }

    mxlMonitoringFlow toHandle(Flow* flow) noexcept
    {
        return reinterpret_cast<mxlMonitoringFlow>(flow);
    }

    Monitoring* toMonitoring(mxlMonitoring monitoring) noexcept
    {
        return reinterpret_cast<Monitoring*>(monitoring);
    }

    void copyText(std::string_view text, char* buffer, std::size_t size) noexcept
    {
        if (buffer == nullptr || size == 0)
        {
            return;
        }
        auto const n = std::min(text.size(), size - 1);
        std::memcpy(buffer, text.data(), n);
        buffer[n] = '\0';
    }

    std::string domainOf(Flow const& flow)
    {
        auto const lock = std::lock_guard{flow.mutex};
        return flow.domain;
    }

    std::string domainUnavailable(std::string const& domain)
    {
        if (domain.empty())
        {
            return {};
        }
        struct stat st{};
        if (::stat(domain.c_str(), &st) != 0)
        {
            return std::format("MXL domain {} does not exist", domain);
        }
        if (!S_ISDIR(st.st_mode) || ::access(domain.c_str(), R_OK | X_OK) != 0)
        {
            return std::format("cannot open MXL domain {}", domain);
        }
        return {};
    }

    bool permissionDenied(std::string const& domain, std::string const& flowId)
    {
        if (domain.empty())
        {
            return false;
        }
        auto const dir = std::format("{}/{}.mxl-flow", domain, flowId);
        auto const denied = [](std::string const& path, int mode)
        {
            struct stat st{};
            return ::stat(path.c_str(), &st) == 0 && ::access(path.c_str(), mode) != 0 && (errno == EACCES || errno == EPERM);
        };
        // A directory has to be traversable as well as readable.
        if (denied(dir, R_OK | X_OK) || denied(dir + "/grains", R_OK | X_OK))
        {
            return true;
        }
        for (auto const& path : {dir + "/flow_def.json", dir + "/data", dir + "/grains/data.0"})
        {
            if (denied(path, R_OK))
            {
                return true;
            }
        }
        return false;
    }
}

using namespace mxl::monitoring;

extern "C"
{
    mxlMonitoringAccess mxlMonitoringClassifyAccess(mxlStatus status)
    {
        switch (status)
        {
            case MXL_STATUS_OK:               return MXL_MONITORING_ACCESS_OPEN;
            case MXL_ERR_FLOW_NOT_FOUND:
            case MXL_ERR_NOT_FOUND:           return MXL_MONITORING_ACCESS_FLOW_NOT_FOUND;
            case MXL_ERR_PERMISSION_DENIED:   return MXL_MONITORING_ACCESS_PERMISSION_DENIED;
            case MXL_ERR_FLOW_INVALID:
            case MXL_ERR_INVALID_FLOW_READER:
            case MXL_ERR_INVALID_FLOW_WRITER: return MXL_MONITORING_ACCESS_FLOW_INVALID;
            default:                          return MXL_MONITORING_ACCESS_OTHER;
        }
    }

    mxlMonitoringRead mxlMonitoringClassifyRead(mxlStatus status)
    {
        switch (status)
        {
            case MXL_STATUS_OK:                 return MXL_MONITORING_READ_OK;
            case MXL_ERR_OUT_OF_RANGE_TOO_EARLY:
            case MXL_ERR_TIMEOUT:               return MXL_MONITORING_READ_TOO_EARLY;
            case MXL_ERR_OUT_OF_RANGE_TOO_LATE: return MXL_MONITORING_READ_TOO_LATE;
            case MXL_ERR_PERMISSION_DENIED:     return MXL_MONITORING_READ_PERMISSION_DENIED;
            default:                            return MXL_MONITORING_READ_ERROR;
        }
    }

    mxlMonitoringWrite mxlMonitoringClassifyWrite(mxlStatus status)
    {
        switch (status)
        {
            case MXL_STATUS_OK:             return MXL_MONITORING_WRITE_OK;
            case MXL_ERR_PERMISSION_DENIED: return MXL_MONITORING_WRITE_PERMISSION_DENIED;
            default:                        return MXL_MONITORING_WRITE_ERROR;
        }
    }

    mxlStatus mxlMonitoringCreate(mxlMonitoringConfig const* config, mxlMonitoring* monitoring)
    {
        if (monitoring == nullptr || (config != nullptr && (config->version != 1 || config->size < sizeof(mxlMonitoringConfig))))
        {
            return MXL_ERR_INVALID_ARG;
        }
        try
        {
            auto defaults = mxlMonitoringConfig{};
            defaults.version = 1;
            defaults.size = sizeof(mxlMonitoringConfig);
            *monitoring = reinterpret_cast<mxlMonitoring>(new Monitoring{config != nullptr ? *config : defaults});
            return MXL_STATUS_OK;
        }
        catch (...)
        {
            return MXL_ERR_UNKNOWN;
        }
    }

    mxlStatus mxlMonitoringDestroy(mxlMonitoring monitoring)
    {
        if (monitoring == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        delete toMonitoring(monitoring);
        return MXL_STATUS_OK;
    }

    mxlStatus mxlMonitoringAddFlow(mxlMonitoring monitoring, char const* flowId, mxlMonitoringRole role, mxlMonitoringFlow* flow)
    {
        auto* const m = toMonitoring(monitoring);
        if (m == nullptr || flowId == nullptr || flow == nullptr || !inRange(static_cast<int>(role), MXL_MONITORING_ROLE_REPLICATION_TARGET + 1))
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const id = std::string_view{flowId};
        if (id.empty() || id.size() >= MXL_MONITORING_ID)
        {
            return MXL_ERR_INVALID_ARG;
        }
        try
        {
            auto const lock = std::lock_guard{m->flowsMutex};
            for (auto const& existing : m->flows)
            {
                if (existing->id == id && existing->role == role)
                {
                    return MXL_ERR_EXISTS;
                }
            }
            m->flows.push_back(std::make_unique<Flow>(*m, std::string{id}, role));
            m->flows.back()->target = id;
            *flow = toHandle(m->flows.back().get());
            return MXL_STATUS_OK;
        }
        catch (...)
        {
            return MXL_ERR_UNKNOWN;
        }
    }

    mxlStatus mxlMonitoringRemoveFlow(mxlMonitoring monitoring, mxlMonitoringFlow flow)
    {
        auto* const m = toMonitoring(monitoring);
        if (m == nullptr || flow == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const pollLock = std::lock_guard{m->pollMutex};
        auto const lock = std::lock_guard{m->flowsMutex};
        auto const it = std::ranges::find_if(m->flows, [&](auto const& f) { return f.get() == toFlow(flow); });
        if (it == m->flows.end())
        {
            return MXL_ERR_NOT_FOUND;
        }
        m->flows.erase(it);
        return MXL_STATUS_OK;
    }

    mxlStatus mxlMonitoringSetFlowTarget(mxlMonitoringFlow flow, char const* flowId)
    {
        auto* const f = toFlow(flow);
        auto const id = std::string_view{flowId != nullptr ? flowId : ""};
        if (f == nullptr || id.size() >= MXL_MONITORING_ID)
        {
            return MXL_ERR_INVALID_ARG;
        }
        try
        {
            auto const lock = std::lock_guard{f->mutex};
            if (f->target != id)
            {
                f->target = id;
                f->forget();
            }
            return MXL_STATUS_OK;
        }
        catch (...)
        {
            return MXL_ERR_UNKNOWN;
        }
    }

    mxlStatus mxlMonitoringGetLastErrorDetail(mxlMonitoringFlow flow, char* buffer, size_t size)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr || buffer == nullptr || size == 0)
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const lock = std::lock_guard{f->mutex};
        copyText(f->lastErrorDetail, buffer, size);
        return MXL_STATUS_OK;
    }

    mxlMonitoringAccess mxlMonitoringGetFlowAccess(mxlMonitoringFlow flow)
    {
        auto* const f = toFlow(flow);
        return f != nullptr ? static_cast<mxlMonitoringAccess>(f->access.load(Relaxed)) : MXL_MONITORING_ACCESS_STARTING;
    }

    char const* mxlMonitoringAccessName(mxlMonitoringAccess access)
    {
        switch (access)
        {
            case MXL_MONITORING_ACCESS_STARTING:           return "starting";
            case MXL_MONITORING_ACCESS_OPEN:               return "open";
            case MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE: return "domain_unavailable";
            case MXL_MONITORING_ACCESS_FLOW_NOT_FOUND:
            case MXL_MONITORING_ACCESS_FLOW_INVALID:       return "flow_not_found";
            case MXL_MONITORING_ACCESS_PERMISSION_DENIED:  return "permission_denied";
            case MXL_MONITORING_ACCESS_OTHER:              return "flow_error";
            case MXL_MONITORING_ACCESS_DISCONNECTED:       return "disconnected";
            case MXL_MONITORING_ACCESS_COUNT:              break;
        }
        return "unknown";
    }

    char const* mxlMonitoringContentName(mxlMonitoringContent content)
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
            case MXL_MONITORING_CONTENT_COUNT:            break;
        }
        return nullptr;
    }

    char const* mxlMonitoringReadName(mxlMonitoringRead read)
    {
        switch (read)
        {
            case MXL_MONITORING_READ_OK:                return "ok";
            case MXL_MONITORING_READ_TOO_EARLY:         return "too_early";
            case MXL_MONITORING_READ_TOO_LATE:          return "too_late";
            case MXL_MONITORING_READ_PERMISSION_DENIED: return "permission_denied";
            case MXL_MONITORING_READ_ERROR:             return "read_error";
            case MXL_MONITORING_READ_INVALID_CONTENT:   return "invalid_content";
            case MXL_MONITORING_READ_COUNT:             break;
        }
        return "unknown";
    }

    char const* mxlMonitoringWriteName(mxlMonitoringWrite write)
    {
        switch (write)
        {
            case MXL_MONITORING_WRITE_OK:                return "ok";
            case MXL_MONITORING_WRITE_INPUT_LATE:        return "input_late";
            case MXL_MONITORING_WRITE_INVALID_INPUT:     return "invalid_input";
            case MXL_MONITORING_WRITE_PERMISSION_DENIED: return "permission_denied";
            case MXL_MONITORING_WRITE_OUT_OF_SPACE:      return "out_of_space";
            case MXL_MONITORING_WRITE_ERROR:             return "write_error";
            case MXL_MONITORING_WRITE_COUNT:             break;
        }
        return "unknown";
    }

    mxlStatus mxlMonitoringDescribe(mxlMonitoringFlow flow, mxlMonitoringConditionKind kind, int outcome, mxlMonitoringContent content,
        char const* detail)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr || detail == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        try
        {
            switch (kind)
            {
                case MXL_MONITORING_CONDITION_ACCESS:
                    if (!inRange(outcome, MXL_MONITORING_ACCESS_COUNT))
                    {
                        return MXL_ERR_INVALID_ARG;
                    }
                    f->describeAccess(static_cast<mxlMonitoringAccess>(outcome), detail);
                    return MXL_STATUS_OK;
                case MXL_MONITORING_CONDITION_READ:
                case MXL_MONITORING_CONDITION_WRITE:
                {
                    auto const invalid = kind == MXL_MONITORING_CONDITION_READ ? outcome == MXL_MONITORING_READ_INVALID_CONTENT
                                                                               : outcome == MXL_MONITORING_WRITE_INVALID_INPUT;
                    if (invalid)
                    {
                        if (!inRange(static_cast<int>(content), MXL_MONITORING_CONTENT_COUNT))
                        {
                            return MXL_ERR_INVALID_ARG;
                        }
                        f->describeContent(content, detail);
                    }
                    else
                    {
                        f->describeFailure(detail);
                    }
                    return MXL_STATUS_OK;
                }
            }
            return MXL_ERR_INVALID_ARG;
        }
        catch (...)
        {
            return MXL_ERR_UNKNOWN;
        }
    }

    mxlStatus mxlMonitoringSetFlowFormat(mxlMonitoringFlow flow, mxlMonitoringFlowFormat const* format)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr || format == nullptr || format->version != 1 || format->size < sizeof(mxlMonitoringFlowFormat))
        {
            return MXL_ERR_INVALID_ARG;
        }
        f->unit.store(format->unit, Relaxed);
        f->format.store(format->format, Relaxed);
        f->rateNumerator.store(format->rate.numerator, Relaxed);
        f->rateDenominator.store(format->rate.denominator, Relaxed);
        auto const lock = std::lock_guard{f->mutex};
        f->mediaType = format->mediaType != nullptr ? format->mediaType : "";
        return MXL_STATUS_OK;
    }

    mxlStatus mxlMonitoringSetFlowTiming(mxlMonitoringFlow flow, uint64_t graceNs, uint64_t stallTimeoutNs)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        f->graceNs.store(graceNs, Relaxed);
        f->stallTimeoutNs.store(stallTimeoutNs, Relaxed);
        f->timingSet.store(true, Relaxed);
        return MXL_STATUS_OK;
    }

    void mxlMonitoringRecordAccess(mxlMonitoringFlow flow, mxlMonitoringAccess access, mxlStatus status)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr || !inRange(static_cast<int>(access), MXL_MONITORING_ACCESS_COUNT) || access == MXL_MONITORING_ACCESS_DISCONNECTED)
        {
            return;
        }
        auto const now = f->now();
        if (access == MXL_MONITORING_ACCESS_OPEN)
        {
            add(f->opens, 1);
            f->lastProgressNs.store(now, Relaxed);
            f->earlyCountedIndex.store(std::numeric_limits<std::uint64_t>::max(), Relaxed);
        }
        else if (access != MXL_MONITORING_ACCESS_STARTING)
        {
            f->accessStatus[access].store(status, Relaxed);
            f->error(status, 0, now);
            add(f->accessFailures[access], 1);
            occur(f->accessOccurrences[access], 1);
        }
        f->access.store(access, Relaxed);
    }

    void mxlMonitoringRecordRead(mxlMonitoringFlow flow, mxlStatus status, uint64_t index, uint64_t count)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr)
        {
            return;
        }
        if (isLoss(status))
        {
            f->lose(mxlMonitoringClassifyAccess(status), status);
            return;
        }
        auto const now = f->now();
        auto const read = mxlMonitoringClassifyRead(status);
        switch (read)
        {
            case MXL_MONITORING_READ_OK:
                add(f->reads[read], count);
                f->data(index, now);
                return;

            case MXL_MONITORING_READ_TOO_EARLY:
            {
                auto const progress = f->lastProgressNs.load(Relaxed);
                auto const stallAfter = f->stallTimeoutNs.load(Relaxed);
                if (progress != 0 && stallAfter != 0 && since(now, progress) >= stallAfter && !f->stallCounted.exchange(true, Relaxed))
                {
                    add(f->stalls, 1);
                }
                // Without a rate nothing says when the index is due.
                auto const r = f->rate();
                if (r.numerator <= 0 || r.denominator <= 0 || f->earlyCountedIndex.load(Relaxed) == index ||
                    now <= mxlIndexToTimestamp(&r, index) + f->graceNs.load(Relaxed))
                {
                    return;
                }
                f->earlyCountedIndex.store(index, Relaxed);
                break;
            }

            case MXL_MONITORING_READ_TOO_LATE: break;

            default: f->access.store(accessAfter(read), Relaxed); break;
        }
        f->readIndex[read].store(index, Relaxed);
        f->readStatus[read].store(status, Relaxed);
        f->error(status, index, now);
        occur(f->reads[read], 1);
    }

    void mxlMonitoringRecordReadContent(mxlMonitoringFlow flow, mxlMonitoringContent issue, uint64_t index, uint64_t count)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr || !inRange(static_cast<int>(issue), MXL_MONITORING_CONTENT_COUNT))
        {
            return;
        }
        auto const now = f->now();
        f->contentIndex[issue].store(index, Relaxed);
        f->error(MXL_STATUS_OK, index, now);
        f->data(index, now);
        add(f->reads[MXL_MONITORING_READ_INVALID_CONTENT], count);
        occur(f->content[issue], count);
    }

    void mxlMonitoringRecordSkipped(mxlMonitoringFlow flow, uint64_t count)
    {
        if (auto* const f = toFlow(flow); f != nullptr)
        {
            add(f->skipped, count);
        }
    }

    void mxlMonitoringRecordCommit(mxlMonitoringFlow flow, uint64_t index, uint64_t commitTimeNs)
    {
        if (auto* const f = toFlow(flow); f != nullptr && f->rate().numerator > 0 && f->rate().denominator > 0)
        {
            f->commitLatency.observe(f->latency(index, commitTimeNs));
        }
    }

    void mxlMonitoringRecordWrite(mxlMonitoringFlow flow, mxlStatus status, uint64_t index, uint64_t count)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr)
        {
            return;
        }
        if (isLoss(status))
        {
            f->lose(mxlMonitoringClassifyAccess(status), status);
            return;
        }
        auto const now = f->now();
        auto const write = mxlMonitoringClassifyWrite(status);
        if (write == MXL_MONITORING_WRITE_OK)
        {
            add(f->writes[write], count);
            f->data(index, now);
            return;
        }
        f->access.store(write == MXL_MONITORING_WRITE_PERMISSION_DENIED ? MXL_MONITORING_ACCESS_PERMISSION_DENIED : MXL_MONITORING_ACCESS_OTHER,
            Relaxed);
        f->writeIndex[write].store(index, Relaxed);
        f->writeStatus[write].store(status, Relaxed);
        f->error(status, index, now);
        occur(f->writes[write], 1);
    }

    void mxlMonitoringRecordWriteOutcome(mxlMonitoringFlow flow, mxlMonitoringWrite outcome, mxlStatus status, uint64_t index, uint64_t count)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr || (outcome != MXL_MONITORING_WRITE_INPUT_LATE && outcome != MXL_MONITORING_WRITE_OUT_OF_SPACE))
        {
            return;
        }
        auto const now = f->now();
        f->writeIndex[outcome].store(index, Relaxed);
        f->writeStatus[outcome].store(status, Relaxed);
        f->error(status, index, now);
        occur(f->writes[outcome], count);
    }

    void mxlMonitoringRecordWriteContent(mxlMonitoringFlow flow, mxlMonitoringContent issue, uint64_t index, uint64_t count)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr || !inRange(static_cast<int>(issue), MXL_MONITORING_CONTENT_COUNT))
        {
            return;
        }
        f->contentIndex[issue].store(index, Relaxed);
        f->error(MXL_STATUS_OK, index, f->now());
        add(f->writes[MXL_MONITORING_WRITE_INVALID_INPUT], count);
        occur(f->content[issue], count);
    }

    void mxlMonitoringRecordInput(mxlMonitoringFlow flow, uint64_t index, uint64_t arrivalNs)
    {
        if (auto* const f = toFlow(flow); f != nullptr && f->rate().numerator > 0 && f->rate().denominator > 0)
        {
            f->inputLatency.observe(f->latency(index, arrivalNs));
        }
    }

    uint64_t mxlMonitoringFlowLastProgress(mxlMonitoringFlow flow)
    {
        auto* const f = toFlow(flow);
        return f != nullptr ? f->lastProgressNs.load(Relaxed) : 0;
    }

    uint64_t mxlMonitoringLatencyBound(size_t bucket)
    {
        return bucket < Bounds.size() ? Bounds[bucket] : std::numeric_limits<std::uint64_t>::max();
    }

    mxlStatus mxlMonitoringSnapshot(mxlMonitoring monitoring, mxlMonitoringFlowSnapshot* snapshots, size_t* count)
    {
        auto* const m = toMonitoring(monitoring);
        if (m == nullptr || count == nullptr || (snapshots == nullptr && *count != 0))
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const now = m->now();
        auto const lock = std::lock_guard{m->flowsMutex};
        auto const n = std::min(*count, m->flows.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            auto const& f = *m->flows[i];
            auto& s = snapshots[i];
            s = mxlMonitoringFlowSnapshot{};
            s.version = 2;
            s.size = sizeof(mxlMonitoringFlowSnapshot);
            copyText(f.id, s.flowId, sizeof(s.flowId));
            s.role = f.role;
            s.unit = static_cast<mxlMonitoringUnit>(f.unit.load(Relaxed));
            s.format = static_cast<mxlDataFormat>(f.format.load(Relaxed));
            s.rate = f.rate();
            s.access = static_cast<mxlMonitoringAccess>(f.access.load(Relaxed));
            for (std::size_t a = 0; a < f.accessFailures.size(); ++a)
            {
                s.accessFailures[a] = f.accessFailures[a].load(Relaxed);
            }
            s.opens = f.opens.load(Relaxed);
            s.losses = f.losses.load(Relaxed);
            for (std::size_t r = 0; r < f.reads.size(); ++r)
            {
                s.reads[r] = f.reads[r].load(Relaxed);
            }
            for (std::size_t w = 0; w < f.writes.size(); ++w)
            {
                s.writes[w] = f.writes[w].load(Relaxed);
            }
            for (std::size_t c = 0; c < f.content.size(); ++c)
            {
                s.content[c] = f.content[c].load(Relaxed);
            }
            s.skipped = f.skipped.load(Relaxed);
            s.stalls = f.stalls.load(Relaxed);
            s.lastErrorStatus = static_cast<mxlStatus>(f.lastErrorStatus.load(Relaxed));
            s.lastErrorIndex = f.lastErrorIndex.load(Relaxed);
            s.lastErrorTimeNs = f.lastErrorTimeNs.load(Relaxed);
            f.arrivalLatency.copyTo(s.arrivalLatency);
            f.commitLatency.copyTo(s.commitLatency);
            f.inputLatency.copyTo(s.inputLatency);
            s.lastIndex = f.lastIndex.load(Relaxed);
            s.lastDataTimeNs = f.lastDataTimeNs.load(Relaxed);
            s.ageNs = f.ageNs.load(Relaxed);
            s.delivering = f.delivering(now);
            auto const flowLock = std::lock_guard{f.mutex};
            copyText(f.mediaType, s.mediaType, sizeof(s.mediaType));
            copyText(f.target, s.target, sizeof(s.target));
            copyText(f.lastErrorDetail, s.lastErrorDetail, sizeof(s.lastErrorDetail));
            copyText(f.formatMismatch, s.formatMismatch, sizeof(s.formatMismatch));
        }
        *count = m->flows.size();
        return MXL_STATUS_OK;
    }
}
