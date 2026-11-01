#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include "mxl-monitoring/bcp008.h"

namespace
{
    bool receiver(mxlMonitoringRole role)
    {
        return role == MXL_MONITORING_ROLE_READER || role == MXL_MONITORING_ROLE_REPLICATION_SOURCE;
    }

    // A text field of a snapshot, which a Node may have filled from another
    // process's counts: never read past its array.
    template<std::size_t N>
    std::string_view text(char const (&field)[N])
    {
        return {field, strnlen(field, N)};
    }

    bool valid(mxlMonitoringFlowSnapshot const* s)
    {
        return s->version == 2 && s->size >= sizeof(mxlMonitoringFlowSnapshot);
    }

    bool sameFlow(mxlMonitoringFlowSnapshot const* earlier, mxlMonitoringFlowSnapshot const& current)
    {
        return earlier == nullptr || (valid(earlier) && earlier->role == current.role && text(earlier->flowId) == text(current.flowId));
    }

    // Every count of a snapshot is monotonic within one process, so any of
    // them lower than before means the process started again.
    bool restarted(mxlMonitoringFlowSnapshot const& earlier, mxlMonitoringFlowSnapshot const& current)
    {
        auto lower = [](auto const& before, auto const& now)
        {
            for (std::size_t i = 0; i < std::size(now); ++i)
            {
                if (now[i] < before[i])
                {
                    return true;
                }
            }
            return false;
        };
        return lower(earlier.reads, current.reads) || lower(earlier.writes, current.writes) || lower(earlier.content, current.content) ||
               lower(earlier.accessFailures, current.accessFailures) || current.opens < earlier.opens ||
               current.losses < earlier.losses || current.skipped < earlier.skipped || current.stalls < earlier.stalls;
    }

    // Counts since the earlier snapshot. None without one; all of them after
    // a restart, which started every count from zero.
    struct Delta
    {
        Delta(mxlMonitoringFlowSnapshot const* earlier, mxlMonitoringFlowSnapshot const& current, bool sinceStart)
            : current{current}
        {
            static auto const zero = mxlMonitoringFlowSnapshot{};
            if (earlier == nullptr)
            {
                none = !sinceStart;
                this->earlier = &zero;
            }
            else
            {
                this->earlier = restarted(*earlier, current) ? &zero : earlier;
            }
        }

        std::uint64_t operator()(std::uint64_t mxlMonitoringFlowSnapshot::* field) const
        {
            return none ? 0 : current.*field - earlier->*field;
        }

        std::uint64_t read(mxlMonitoringRead outcome) const
        {
            return none ? 0 : current.reads[outcome] - earlier->reads[outcome];
        }

        std::uint64_t write(mxlMonitoringWrite outcome) const
        {
            return none ? 0 : current.writes[outcome] - earlier->writes[outcome];
        }

        std::uint64_t content(mxlMonitoringContent issue) const
        {
            return none ? 0 : current.content[issue] - earlier->content[issue];
        }

        mxlMonitoringFlowSnapshot const* earlier;
        mxlMonitoringFlowSnapshot const& current;
        bool none = false;
    };

    char const* units(mxlMonitoringFlowSnapshot const& s, std::uint64_t count)
    {
        if (s.unit == MXL_MONITORING_UNIT_SAMPLE)
        {
            return count == 1 ? "sample" : "samples";
        }
        return count == 1 ? "grain" : "grains";
    }

    char const* plural(std::uint64_t count, char const* one, char const* many)
    {
        return count == 1 ? one : many;
    }

    // Cut to the field, never inside a UTF-8 sequence: the message ends up
    // in an IS-12 JSON string.
    void set(mxlMonitoringBcp008Domain& domain, mxlMonitoringBcp008Status status, std::string_view message = {})
    {
        domain.status = status;
        auto length = message.size();
        if (length > sizeof(domain.message) - 1)
        {
            length = sizeof(domain.message) - 1;
            while (length > 0 && (static_cast<unsigned char>(message[length]) & 0xC0) == 0x80)
            {
                --length;
            }
        }
        std::memcpy(domain.message, message.data(), length);
        domain.message[length] = '\0';
    }

    // Why the flow cannot be used now, or nothing while it is open and
    // delivers.
    std::string unreachable(mxlMonitoringFlowSnapshot const& s, char const* idle)
    {
        if (s.access != MXL_MONITORING_ACCESS_OPEN)
        {
            auto out = std::format("flow {}", mxlMonitoringAccessName(s.access));
            if (auto const detail = text(s.lastErrorDetail); s.access != MXL_MONITORING_ACCESS_STARTING && !detail.empty())
            {
                out += std::format(": {}", detail);
            }
            return out;
        }
        return s.delivering ? std::string{} : std::string{idle};
    }

    // What went wrong on the way since the earlier snapshot, worst first,
    // including failures already recovered from: the poll comes too late to
    // see them as the flow's state.
    std::string interrupted(Delta const& delta, mxlMonitoringFlowSnapshot const& s)
    {
        if (auto const losses = delta(&mxlMonitoringFlowSnapshot::losses); losses > 0)
        {
            return std::format("flow went away {} {}", losses, plural(losses, "time", "times"));
        }
        if (auto const stalls = delta(&mxlMonitoringFlowSnapshot::stalls); stalls > 0)
        {
            return std::format("{} {}", stalls, plural(stalls, "stall", "stalls"));
        }
        if (receiver(s.role))
        {
            if (auto const failed = delta.read(MXL_MONITORING_READ_PERMISSION_DENIED) + delta.read(MXL_MONITORING_READ_ERROR); failed > 0)
            {
                return std::format("{} failed {}", failed, plural(failed, "read", "reads"));
            }
            if (auto const lost = delta(&mxlMonitoringFlowSnapshot::skipped) + delta.content(MXL_MONITORING_CONTENT_MISSING); lost > 0)
            {
                return std::format("{} {} lost", lost, units(s, lost));
            }
            if (auto const late = delta.read(MXL_MONITORING_READ_TOO_EARLY); late > 0)
            {
                return std::format("{} late {}", late, plural(late, "read", "reads"));
            }
            return {};
        }
        static constexpr mxlMonitoringWrite Failures[] = {
            MXL_MONITORING_WRITE_PERMISSION_DENIED,
            MXL_MONITORING_WRITE_OUT_OF_SPACE,
            MXL_MONITORING_WRITE_ERROR,
        };
        auto failed = std::uint64_t{0};
        auto kinds = std::string{};
        for (auto const failure : Failures)
        {
            if (auto const count = delta.write(failure); count > 0)
            {
                failed += count;
                kinds += std::format("{}{} {}", kinds.empty() ? "" : ", ", count, mxlMonitoringWriteName(failure));
            }
        }
        return failed == 0 ? std::string{} : std::format("{} failed {}: {}", failed, plural(failed, "commit", "commits"), kinds);
    }

    void connection(Delta const& delta, mxlMonitoringFlowSnapshot const& s, mxlMonitoringBcp008Statuses& out)
    {
        auto why = unreachable(s, receiver(s.role) ? "nothing read within the last second" : "nothing committed within the last second");
        if (why.empty())
        {
            why = interrupted(delta, s);
        }
        set(out.connection, why.empty() ? MXL_MONITORING_BCP008_HEALTHY : MXL_MONITORING_BCP008_UNHEALTHY, why);
    }

    void receiverStream(Delta const& delta, mxlMonitoringFlowSnapshot const& s, mxlMonitoringBcp008Statuses& out)
    {
        // Worst first: what cannot be decoded, then what decodes but is not
        // what was expected. Missing grains were lost, not wrong.
        static constexpr mxlMonitoringContent Issues[] = {
            MXL_MONITORING_CONTENT_WRONG_FORMAT,
            MXL_MONITORING_CONTENT_WRONG_SIZE,
            MXL_MONITORING_CONTENT_INVALID,
            MXL_MONITORING_CONTENT_WRONG_RESOLUTION,
            MXL_MONITORING_CONTENT_WRONG_RATE,
            MXL_MONITORING_CONTENT_INCOMPLETE,
            MXL_MONITORING_CONTENT_ILLEGAL_VALUES,
        };
        for (auto const issue : Issues)
        {
            if (auto const count = delta.content(issue); count > 0)
            {
                auto const unhealthy = issue == MXL_MONITORING_CONTENT_WRONG_FORMAT || issue == MXL_MONITORING_CONTENT_WRONG_SIZE ||
                                       issue == MXL_MONITORING_CONTENT_INVALID;
                auto message = std::format("{} {} {}", count, units(s, count), mxlMonitoringContentName(issue));
                auto const mismatch = text(s.formatMismatch);
                if (!mismatch.empty() && (issue == MXL_MONITORING_CONTENT_WRONG_FORMAT || issue == MXL_MONITORING_CONTENT_WRONG_RESOLUTION ||
                                             issue == MXL_MONITORING_CONTENT_WRONG_RATE))
                {
                    message += std::format(": {}", mismatch);
                }
                set(out.stream, unhealthy ? MXL_MONITORING_BCP008_UNHEALTHY : MXL_MONITORING_BCP008_PARTIALLY_HEALTHY, message);
                return;
            }
        }
        // A missing grain counts as invalid content but was never there to
        // decode.
        auto const read = delta.read(MXL_MONITORING_READ_OK) + delta.read(MXL_MONITORING_READ_INVALID_CONTENT);
        auto const missing = delta.content(MXL_MONITORING_CONTENT_MISSING);
        set(out.stream, read > missing ? MXL_MONITORING_BCP008_HEALTHY : MXL_MONITORING_BCP008_UNCHANGED);
    }

    void senderEssence(Delta const& delta, mxlMonitoringFlowSnapshot const& s, mxlMonitoringBcp008Statuses& out)
    {
        auto const written = delta.write(MXL_MONITORING_WRITE_OK);
        auto const invalid = delta.write(MXL_MONITORING_WRITE_INVALID_INPUT);
        auto const late = delta.write(MXL_MONITORING_WRITE_INPUT_LATE);
        if (invalid > 0)
        {
            auto message = std::format("{} {} of invalid input", invalid, units(s, invalid));
            for (auto issue = 0; issue < MXL_MONITORING_CONTENT_COUNT; ++issue)
            {
                if (delta.content(static_cast<mxlMonitoringContent>(issue)) > 0)
                {
                    message += std::format(", {}", mxlMonitoringContentName(static_cast<mxlMonitoringContent>(issue)));
                    break;
                }
            }
            set(out.stream, MXL_MONITORING_BCP008_UNHEALTHY, message);
        }
        else if (late > 0)
        {
            // No input at all is no essence; some input in time is essence
            // with gaps.
            set(out.stream, written > 0 ? MXL_MONITORING_BCP008_PARTIALLY_HEALTHY : MXL_MONITORING_BCP008_UNHEALTHY,
                std::format("{} {} without input in time", late, units(s, late)));
        }
        else
        {
            set(out.stream, written > 0 ? MXL_MONITORING_BCP008_HEALTHY : MXL_MONITORING_BCP008_UNCHANGED);
        }
    }

    struct Counter
    {
        char const* name;
        char const* description;
        std::uint64_t (*value)(Delta const&);
    };

    constexpr Counter Lost[] = {
        {"skipped", "Grains or samples jumped over after the ring overtook the reader",
         [](Delta const& d) { return d(&mxlMonitoringFlowSnapshot::skipped); }                                 },
        {"missing", "Grains or samples whose ring slot held another index: never written",
         [](Delta const& d) { return d.content(MXL_MONITORING_CONTENT_MISSING); }                              },
    };

    constexpr Counter Late[] = {
        {"too_early", "Reads that found their grain or samples not there a grace period past the media timestamp",
         [](Delta const& d) { return d.read(MXL_MONITORING_READ_TOO_EARLY); }                                  },
    };

    constexpr Counter TransmissionErrors[] = {
        {"permission_denied", "Commits refused for lack of permission",
         [](Delta const& d) { return d.write(MXL_MONITORING_WRITE_PERMISSION_DENIED); }                        },
        {"out_of_space",      "Attempts to write that found the domain full",
         [](Delta const& d) { return d.write(MXL_MONITORING_WRITE_OUT_OF_SPACE); }                             },
        {"write_error",       "Commits that failed otherwise",
         [](Delta const& d) { return d.write(MXL_MONITORING_WRITE_ERROR); }                                    },
    };
}

extern "C"
{
    bool mxlMonitoringBcp008Restarted(mxlMonitoringFlowSnapshot const* previous, mxlMonitoringFlowSnapshot const* current)
    {
        return previous != nullptr && current != nullptr && restarted(*previous, *current);
    }

    mxlStatus mxlMonitoringBcp008GetStatuses(mxlMonitoringFlowSnapshot const* previous, mxlMonitoringFlowSnapshot const* current,
        mxlMonitoringBcp008Statuses* statuses)
    {
        if (current == nullptr || !valid(current) || statuses == nullptr || statuses->version != 1 ||
            statuses->size < sizeof(mxlMonitoringBcp008Statuses) || !sameFlow(previous, *current))
        {
            return MXL_ERR_INVALID_ARG;
        }
        try
        {
            auto& out = *statuses;
            auto const& s = *current;
            if (s.access == MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE)
            {
                set(out.link, MXL_MONITORING_BCP008_UNHEALTHY, "MXL domain unavailable");
            }
            else
            {
                set(out.link, MXL_MONITORING_BCP008_HEALTHY);
            }
            set(out.synchronization, MXL_MONITORING_BCP008_INACTIVE);
            if (s.access == MXL_MONITORING_ACCESS_DISCONNECTED)
            {
                set(out.connection, MXL_MONITORING_BCP008_INACTIVE);
                set(out.stream, MXL_MONITORING_BCP008_INACTIVE);
                return MXL_STATUS_OK;
            }
            auto const delta = Delta{previous, s, false};
            connection(delta, s, out);
            if (receiver(s.role))
            {
                receiverStream(delta, s, out);
            }
            else
            {
                senderEssence(delta, s, out);
            }
            return MXL_STATUS_OK;
        }
        catch (...)
        {
            return MXL_ERR_UNKNOWN;
        }
    }

    mxlStatus mxlMonitoringBcp008GetCounters(mxlMonitoringFlowSnapshot const* baseline, mxlMonitoringFlowSnapshot const* current,
        mxlMonitoringBcp008Counters which, mxlMonitoringBcp008Counter* counters, size_t* count)
    {
        if (current == nullptr || !valid(current) || count == nullptr || (counters == nullptr && *count > 0) || !sameFlow(baseline, *current))
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const* table = static_cast<Counter const*>(nullptr);
        auto size = std::size_t{0};
        auto const onReceiver = receiver(current->role);
        if (which == MXL_MONITORING_BCP008_LOST && onReceiver)
        {
            table = Lost;
            size = std::size(Lost);
        }
        else if (which == MXL_MONITORING_BCP008_LATE && onReceiver)
        {
            table = Late;
            size = std::size(Late);
        }
        else if (which == MXL_MONITORING_BCP008_TRANSMISSION_ERROR && !onReceiver)
        {
            table = TransmissionErrors;
            size = std::size(TransmissionErrors);
        }
        auto const delta = Delta{baseline, *current, true};
        for (std::size_t i = 0; i < size && i < *count; ++i)
        {
            counters[i] = mxlMonitoringBcp008Counter{table[i].name, table[i].description, table[i].value(delta)};
        }
        *count = size;
        return MXL_STATUS_OK;
    }
}
