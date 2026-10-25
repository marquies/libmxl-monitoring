#include <cstring>
#include <format>
#include <string>
#include <vector>
#include "mxl-monitoring/prometheus.h"

namespace
{
    // A label value from a writer-controlled string: anything outside a media
    // type's own characters would break the exposition.
    std::string labelValue(char const* value)
    {
        auto out = std::string{};
        for (auto const* c = value; *c != '\0'; ++c)
        {
            auto const ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '/' || *c == '.' ||
                            *c == '_' || *c == '+' || *c == '-';
            out += ok ? *c : '_';
        }
        return out;
    }

    char const* prefix(mxlMonitoringRole role)
    {
        switch (role)
        {
            case MXL_MONITORING_ROLE_READER:             return "mxl_reader";
            case MXL_MONITORING_ROLE_WRITER:             return "mxl_writer";
            case MXL_MONITORING_ROLE_REPLICATION_SOURCE: return "mxl_gateway_source";
            case MXL_MONITORING_ROLE_REPLICATION_TARGET: return "mxl_gateway_target";
        }
        return "mxl_unknown";
    }

    bool readSide(mxlMonitoringRole role)
    {
        return role == MXL_MONITORING_ROLE_READER || role == MXL_MONITORING_ROLE_REPLICATION_SOURCE;
    }

    struct Key
    {
        char const* key;
        std::uint64_t (*value)(mxlMonitoringFlowSnapshot const&);
    };

    std::uint64_t wrongFormat(mxlMonitoringFlowSnapshot const& s)
    {
        return s.content[MXL_MONITORING_CONTENT_WRONG_FORMAT] + s.content[MXL_MONITORING_CONTENT_WRONG_RESOLUTION] +
               s.content[MXL_MONITORING_CONTENT_WRONG_RATE];
    }

    // mxl-reader's read results; wrong_format covers every way the flow
    // differs from its expectation.
    constexpr Key ReadResults[] = {
        {"ok",               [](auto const& s) { return s.reads[MXL_MONITORING_READ_OK]; }                  },
        {"too_early",        [](auto const& s) { return s.reads[MXL_MONITORING_READ_TOO_EARLY]; }           },
        {"too_late",         [](auto const& s) { return s.reads[MXL_MONITORING_READ_TOO_LATE]; }            },
        {"missing",          [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_MISSING]; }        },
        {"invalid",          [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_INVALID]; }        },
        {"incomplete",       [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_INCOMPLETE]; }     },
        {"wrong_size",       [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_WRONG_SIZE]; }     },
        {"wrong_format",     wrongFormat                                                                    },
        {"illegal_values",   [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_ILLEGAL_VALUES]; } },
        {"permission_error", [](auto const& s) { return s.reads[MXL_MONITORING_READ_PERMISSION_DENIED]; }   },
        {"other_error",      [](auto const& s) { return s.reads[MXL_MONITORING_READ_ERROR]; }               },
    };

    constexpr Key WriteResults[] = {
        {"ok",               [](auto const& s) { return s.writes[MXL_MONITORING_WRITE_OK]; }                },
        {"input_late",       [](auto const& s) { return s.writes[MXL_MONITORING_WRITE_INPUT_LATE]; }        },
        {"invalid_input",    [](auto const& s) { return s.writes[MXL_MONITORING_WRITE_INVALID_INPUT]; }     },
        {"permission_error", [](auto const& s) { return s.writes[MXL_MONITORING_WRITE_PERMISSION_DENIED]; } },
        {"out_of_space",     [](auto const& s) { return s.writes[MXL_MONITORING_WRITE_OUT_OF_SPACE]; }      },
        {"other_error",      [](auto const& s) { return s.writes[MXL_MONITORING_WRITE_ERROR]; }             },
    };

    constexpr Key InputIssues[] = {
        {"wrong_format",     [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_WRONG_FORMAT]; }     },
        {"wrong_resolution", [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_WRONG_RESOLUTION]; } },
        {"wrong_rate",       [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_WRONG_RATE]; }       },
        {"wrong_size",       [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_WRONG_SIZE]; }       },
        {"missing",          [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_MISSING]; }          },
        {"invalid",          [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_INVALID]; }          },
        {"incomplete",       [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_INCOMPLETE]; }       },
        {"illegal_values",   [](auto const& s) { return s.content[MXL_MONITORING_CONTENT_ILLEGAL_VALUES]; }   },
    };

    constexpr Key OpenFailures[] = {
        {"domain",     [](auto const& s) { return s.accessFailures[MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE]; }  },
        {"not_found",  [](auto const& s) { return s.accessFailures[MXL_MONITORING_ACCESS_FLOW_NOT_FOUND]; }      },
        {"permission", [](auto const& s) { return s.accessFailures[MXL_MONITORING_ACCESS_PERMISSION_DENIED]; }   },
        {"other",
         [](auto const& s) { return s.accessFailures[MXL_MONITORING_ACCESS_FLOW_INVALID] + s.accessFailures[MXL_MONITORING_ACCESS_OTHER]; }},
    };

    struct Single
    {
        char const* suffix;
        char const* help;
        std::uint64_t (*value)(mxlMonitoringFlowSnapshot const&);
        bool readOnly;
    };

    constexpr Single Singles[] = {
        {"skipped_total", "Grains or samples jumped over to reach the head again after the ring overtook the reader.",
         [](auto const& s) { return s.skipped; }, true                                                                       },
        {"stalls_total",  "Read timeouts that passed with no read at all.",                                            [](auto const& s) { return s.stalls; },
         true                                                                                                                },
        {"opens_total",   "Times the flow was opened.",                                                                [](auto const& s) { return s.opens; },
         false                                                                                                               },
        {"lost_total",    "Times an open flow went away and was reopened.",                                            [](auto const& s) { return s.losses; },
         false                                                                                                               },
    };

    class Renderer
    {
    public:
        Renderer(std::vector<mxlMonitoringFlowSnapshot const*> flows, char const* prefix, bool reads)
            : _flows{std::move(flows)}
            , _p{prefix}
            , _reads{reads}
        {}

        std::string render()
        {
            info();
            family("flow_state", _reads ? "1 for the state the flow's reader is in." : "1 for the state the flow's writer is in.", "gauge",
                [&](auto const& s) { line(std::format("{}_flow_state{{flow=\"{}\",state=\"{}\"}} 1", _p, id(s), mxlMonitoringAccessName(s.access))); });
            family("flow_delivering", _reads ? "1 while reads land within a second." : "1 while commits land within a second.", "gauge",
                [&](auto const& s) { line(std::format("{}_flow_delivering{{flow=\"{}\"}} {}", _p, id(s), s.delivering ? 1 : 0)); });
            if (_reads)
            {
                keyed("reads_total",
                    "Reads by outcome, in grains (discrete) or samples (continuous); too_early and too_late count events.", "result", ReadResults);
            }
            else
            {
                keyed("writes_total", "Writes by outcome, in grains (discrete) or samples (continuous); permission_error and other_error count events.", "result",
                    WriteResults);
                keyed("invalid_input_total", "Input found wrong before writing, by issue.", "issue", InputIssues);
            }
            keyed("open_failures_total", "Attempts to reach a flow that failed, by reason.", "reason", OpenFailures);
            for (auto const& single : Singles)
            {
                if (single.readOnly && !_reads)
                {
                    continue;
                }
                family(single.suffix, single.help, "counter",
                    [&](auto const& s) { line(std::format("{}_{}{{flow=\"{}\"}} {}", _p, single.suffix, id(s), single.value(s))); });
            }
            family("age_seconds",
                _reads ? "Wall clock minus the media timestamp of the last read." : "Wall clock minus the media timestamp of the last commit.", "gauge",
                [&](auto const& s)
                {
                    if (s.lastDataTimeNs != 0)
                    {
                        line(std::format("{}_age_seconds{{flow=\"{}\"}} {:.6f}", _p, id(s), static_cast<double>(s.ageNs) / 1e9));
                    }
                });
            if (_reads)
            {
                histogram("arrival_seconds", "Read time minus the media timestamp of what was read.", &mxlMonitoringFlowSnapshot::arrivalLatency);
                histogram("commit_seconds", "Commit time minus the media timestamp, for reads of the head.", &mxlMonitoringFlowSnapshot::commitLatency);
            }
            else
            {
                histogram("commit_seconds", "Commit time minus the media timestamp of what was committed.", &mxlMonitoringFlowSnapshot::commitLatency);
                histogram("input_seconds", "Input arrival minus the media timestamp it is written at.", &mxlMonitoringFlowSnapshot::inputLatency);
            }
            return std::move(_out);
        }

    private:
        static std::string id(mxlMonitoringFlowSnapshot const& s)
        {
            return labelValue(s.flowId);
        }

        void line(std::string const& text)
        {
            _out += text;
            _out += '\n';
        }

        template<typename Each>
        void family(char const* suffix, char const* help, char const* type, Each each)
        {
            line(std::format("# HELP {}_{} {}", _p, suffix, help));
            line(std::format("# TYPE {}_{} {}", _p, suffix, type));
            for (auto const* s : _flows)
            {
                each(*s);
            }
        }

        void info()
        {
            family("flow_info", "Media type and unit of a flow, once it has been opened.", "gauge",
                [&](auto const& s)
                {
                    if (s.opens > 0 && s.mediaType[0] != '\0')
                    {
                        line(std::format("{}_flow_info{{flow=\"{}\",media_type=\"{}\",unit=\"{}\"}} 1", _p, id(s), labelValue(s.mediaType),
                            s.unit == MXL_MONITORING_UNIT_SAMPLE ? "samples" : "grains"));
                    }
                });
        }

        template<std::size_t N>
        void keyed(char const* suffix, char const* help, char const* label, Key const (&keys)[N])
        {
            family(suffix, help, "counter",
                [&](auto const& s)
                {
                    for (auto const& k : keys)
                    {
                        line(std::format("{}_{}{{flow=\"{}\",{}=\"{}\"}} {}", _p, suffix, id(s), label, k.key, k.value(s)));
                    }
                });
        }

        void histogram(char const* suffix, char const* help, mxlMonitoringHistogram mxlMonitoringFlowSnapshot::*field)
        {
            family(suffix, help, "histogram",
                [&](auto const& s)
                {
                    auto const& h = s.*field;
                    auto cumulative = std::uint64_t{0};
                    for (std::size_t b = 0; b + 1 < MXL_MONITORING_LATENCY_BUCKETS; ++b)
                    {
                        cumulative += h.buckets[b];
                        line(std::format("{}_{}_bucket{{flow=\"{}\",le=\"{}\"}} {}", _p, suffix, id(s),
                            static_cast<double>(mxlMonitoringLatencyBound(b)) / 1e9, cumulative));
                    }
                    // From the buckets rather than the separately counted
                    // total, so +Inf is never below the last bound.
                    cumulative += h.buckets[MXL_MONITORING_LATENCY_BUCKETS - 1];
                    line(std::format("{}_{}_bucket{{flow=\"{}\",le=\"+Inf\"}} {}", _p, suffix, id(s), cumulative));
                    line(std::format("{}_{}_sum{{flow=\"{}\"}} {:.6f}", _p, suffix, id(s), static_cast<double>(h.sumNs) / 1e9));
                    line(std::format("{}_{}_count{{flow=\"{}\"}} {}", _p, suffix, id(s), cumulative));
                });
        }

        std::vector<mxlMonitoringFlowSnapshot const*> _flows;
        char const* _p;
        bool _reads;
        std::string _out;
    };
}

extern "C"
{
    mxlStatus mxlMonitoringPrometheusRender(mxlMonitoring monitoring, char* buffer, size_t* size)
    {
        if (monitoring == nullptr || size == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        try
        {
            auto count = std::size_t{0};
            auto snapshots = std::vector<mxlMonitoringFlowSnapshot>{};
            // Flows may be added between the two calls; take what fits.
            do
            {
                snapshots.resize(count);
                count = snapshots.size();
                if (auto const status = mxlMonitoringSnapshot(monitoring, snapshots.data(), &count); status != MXL_STATUS_OK)
                {
                    return status;
                }
            }
            while (count > snapshots.size());
            snapshots.resize(count);

            auto out = std::string{};
            for (auto const role :
                {MXL_MONITORING_ROLE_READER, MXL_MONITORING_ROLE_WRITER, MXL_MONITORING_ROLE_REPLICATION_SOURCE, MXL_MONITORING_ROLE_REPLICATION_TARGET})
            {
                auto flows = std::vector<mxlMonitoringFlowSnapshot const*>{};
                for (auto const& s : snapshots)
                {
                    if (s.role == role)
                    {
                        flows.push_back(&s);
                    }
                }
                if (!flows.empty())
                {
                    out += Renderer{std::move(flows), prefix(role), readSide(role)}.render();
                }
            }

            auto const needed = out.size() + 1;
            auto const fits = buffer != nullptr && *size >= needed;
            *size = needed;
            if (!fits)
            {
                return MXL_ERR_INVALID_ARG;
            }
            std::memcpy(buffer, out.c_str(), needed);
            return MXL_STATUS_OK;
        }
        catch (...)
        {
            return MXL_ERR_UNKNOWN;
        }
    }
}
