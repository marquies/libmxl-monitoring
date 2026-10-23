#include <string>
#include "Flow.hpp"

namespace mxl::monitoring
{
    namespace
    {
        constexpr auto Relaxed = std::memory_order_relaxed;

        struct Poller
        {
            Monitoring& monitoring;
            Flow& flow;
            std::uint64_t now;

            // The text of this condition only: timing conditions have none.
            std::string detail(mxlMonitoringConditionKind kind, int outcome, mxlMonitoringContent content) const
            {
                auto const lock = std::lock_guard{flow.mutex};
                if (kind == MXL_MONITORING_CONDITION_ACCESS)
                {
                    return flow.accessDetail[outcome];
                }
                if (content != MXL_MONITORING_CONTENT_COUNT)
                {
                    return flow.contentDetail[content];
                }
                auto const timing = kind == MXL_MONITORING_CONDITION_READ
                                      ? (outcome == MXL_MONITORING_READ_TOO_EARLY || outcome == MXL_MONITORING_READ_TOO_LATE)
                                      : outcome == MXL_MONITORING_WRITE_INPUT_LATE;
                return timing ? std::string{} : flow.failureDetail;
            }

            void emit(Condition const& c, mxlMonitoringConditionKind kind, int outcome, mxlMonitoringContent content, mxlMonitoringPhase phase,
                mxlStatus status, std::uint64_t index, std::uint64_t occurrences)
            {
                auto event = mxlMonitoringEvent{};
                event.version = 1;
                event.size = sizeof(mxlMonitoringEvent);
                event.timeNs = now;
                copyText(flow.id, event.flowId, sizeof(event.flowId));
                event.role = flow.role;
                event.kind = kind;
                event.phase = phase;
                switch (kind)
                {
                    case MXL_MONITORING_CONDITION_ACCESS: event.access = static_cast<mxlMonitoringAccess>(outcome); break;
                    case MXL_MONITORING_CONDITION_READ:   event.read = static_cast<mxlMonitoringRead>(outcome); break;
                    case MXL_MONITORING_CONDITION_WRITE:  event.write = static_cast<mxlMonitoringWrite>(outcome); break;
                }
                event.content = content;
                event.status = status;
                event.index = index;
                event.occurrences = occurrences;
                event.total = c.total;
                event.durationNs = since(now, c.startedAt);
                copyText(detail(kind, outcome, content), event.detail, sizeof(event.detail));
                for (auto const& s : monitoring.subscribers)
                {
                    s.fn(&event, s.user);
                }
            }

            // One step of one condition. count is its counter now, ok the
            // flow's count of good units; recovered says whether the flow is
            // back without regard to time, for access conditions.
            void step(Condition& c, std::uint64_t count, std::uint64_t ok, bool accessCondition, mxlMonitoringConditionKind kind, int outcome,
                mxlMonitoringContent content, mxlStatus status, std::uint64_t index)
            {
                auto const delta = count - c.seen; // counters only grow
                c.seen = count;
                if (delta > 0)
                {
                    c.lastOccurrenceAt = now;
                    c.okAtLastOccurrence = ok;
                    c.total += delta;
                    if (!c.active)
                    {
                        c.active = true;
                        c.startedAt = now;
                        c.lastEventAt = now;
                        c.total = delta;
                        c.pending = 0;
                        emit(c, kind, outcome, content, MXL_MONITORING_PHASE_STARTED, status, index, delta);
                        return;
                    }
                    c.pending += delta;
                }
                if (!c.active)
                {
                    return;
                }
                // An open flow ends an access condition. A read or write
                // condition ends once it has not recurred for the delay and
                // the flow delivers good data again, so a flow that merely
                // went quiet has not recovered.
                auto const delivering = flow.delivering(now);
                auto const recovered = accessCondition ? flow.access.load(Relaxed) == MXL_MONITORING_ACCESS_OPEN
                                                       : delta == 0 && since(now, c.lastOccurrenceAt) >= monitoring.recoverDelayNs &&
                                                             ok > c.okAtLastOccurrence && delivering;
                if (recovered)
                {
                    emit(c, kind, outcome, content, MXL_MONITORING_PHASE_RECOVERED, status, index, c.pending);
                    c.active = false;
                    c.pending = 0;
                    return;
                }
                // A condition persists while it recurs, and while a flow it
                // stopped stays without data: a reader waiting for a writer
                // that went away meets one TOO_EARLY index, then nothing.
                auto const persists = c.pending > 0 || (!accessCondition && !delivering);
                if (persists && since(now, c.lastEventAt) >= monitoring.persistIntervalNs)
                {
                    emit(c, kind, outcome, content, MXL_MONITORING_PHASE_PERSISTING, status, index, c.pending);
                    c.lastEventAt = now;
                    c.pending = 0;
                }
            }

            void run()
            {
                for (int a = MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE; a < MXL_MONITORING_ACCESS_COUNT; ++a)
                {
                    step(flow.accessConditions[a], flow.accessOccurrences[a].load(std::memory_order_acquire), 0, true, MXL_MONITORING_CONDITION_ACCESS, a,
                        MXL_MONITORING_CONTENT_COUNT, static_cast<mxlStatus>(flow.accessStatus[a].load(Relaxed)), 0);
                }

                auto const readSide = flow.role == MXL_MONITORING_ROLE_READER || flow.role == MXL_MONITORING_ROLE_REPLICATION_SOURCE;
                auto const kind = readSide ? MXL_MONITORING_CONDITION_READ : MXL_MONITORING_CONDITION_WRITE;
                auto const ok = readSide ? flow.reads[MXL_MONITORING_READ_OK].load(Relaxed) : flow.writes[MXL_MONITORING_WRITE_OK].load(Relaxed);
                auto const invalid = readSide ? static_cast<int>(MXL_MONITORING_READ_INVALID_CONTENT) : static_cast<int>(MXL_MONITORING_WRITE_INVALID_INPUT);

                if (readSide)
                {
                    for (int r = MXL_MONITORING_READ_TOO_EARLY; r < MXL_MONITORING_READ_COUNT; ++r)
                    {
                        if (r != invalid)
                        {
                            step(flow.readConditions[r], flow.reads[r].load(std::memory_order_acquire), ok, false, kind, r, MXL_MONITORING_CONTENT_COUNT,
                                static_cast<mxlStatus>(flow.readStatus[r].load(Relaxed)), flow.readIndex[r].load(Relaxed));
                        }
                    }
                }
                else
                {
                    for (int w = MXL_MONITORING_WRITE_INPUT_LATE; w < MXL_MONITORING_WRITE_COUNT; ++w)
                    {
                        if (w != invalid)
                        {
                            step(flow.writeConditions[w], flow.writes[w].load(std::memory_order_acquire), ok, false, kind, w, MXL_MONITORING_CONTENT_COUNT,
                                static_cast<mxlStatus>(flow.writeStatus[w].load(Relaxed)), flow.writeIndex[w].load(Relaxed));
                        }
                    }
                }
                for (int c = 0; c < MXL_MONITORING_CONTENT_COUNT; ++c)
                {
                    step(flow.contentConditions[c], flow.content[c].load(std::memory_order_acquire), ok, false, kind, invalid, static_cast<mxlMonitoringContent>(c),
                        MXL_STATUS_OK, flow.contentIndex[c].load(Relaxed));
                }
            }
        };
    }
}

using namespace mxl::monitoring;

extern "C"
{
    mxlStatus mxlMonitoringSubscribe(mxlMonitoring monitoring, mxlMonitoringEventFn fn, void* user)
    {
        auto* const m = toMonitoring(monitoring);
        if (m == nullptr || fn == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        try
        {
            auto const lock = std::lock_guard{m->pollMutex};
            m->subscribers.push_back({fn, user});
            return MXL_STATUS_OK;
        }
        catch (...)
        {
            return MXL_ERR_UNKNOWN;
        }
    }

    mxlStatus mxlMonitoringPoll(mxlMonitoring monitoring)
    {
        auto* const m = toMonitoring(monitoring);
        if (m == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const pollLock = std::lock_guard{m->pollMutex};
        auto flows = std::vector<Flow*>{};
        {
            auto const lock = std::lock_guard{m->flowsMutex};
            for (auto const& f : m->flows)
            {
                flows.push_back(f.get());
            }
        }
        // Flows are removed only under the poll lock, so the copied list stays
        // valid while subscribers run without the list locked.
        auto const now = m->now();
        for (auto* const f : flows)
        {
            Poller{*m, *f, now}.run();
        }
        return MXL_STATUS_OK;
    }
}
