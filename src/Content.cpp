#include <array>
#include <cmath>
#include <format>
#include <string>
#include <vector>
// Inside a dmf-mxl/mxl tree its picojson target offers only this wrapper.
#if __has_include(<picojson/wrapper.h>)
#   include <picojson/wrapper.h>
#else
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#   include <picojson/picojson.h>
#   pragma GCC diagnostic pop
#endif
#include "Flow.hpp"

namespace mxl::monitoring
{
    namespace
    {
        int jsonInt(picojson::object const& object, char const* key)
        {
            auto const it = object.find(key);
            return (it != object.end() && it->second.is<double>()) ? static_cast<int>(it->second.get<double>()) : 0;
        }

        bool unset(mxlRational const& rate)
        {
            return rate.numerator == 0 && rate.denominator == 0;
        }

        // Rates compare as fractions: 50/1 and 100/2 are the same rate.
        bool sameRate(mxlRational const& a, mxlRational const& b)
        {
            return a.numerator * b.denominator == b.numerator * a.denominator;
        }

        std::string rateText(mxlRational const& rate)
        {
            return std::format("{}/{}", rate.numerator, rate.denominator);
        }

        constexpr std::uint32_t bit(mxlMonitoringContent issue)
        {
            return 1U << issue;
        }

        // The worst issue a mismatch stands for, when a grain or batch is
        // counted against it.
        mxlMonitoringContent mismatchIssue(std::uint32_t mismatch)
        {
            for (auto const issue : {MXL_MONITORING_CONTENT_WRONG_FORMAT, MXL_MONITORING_CONTENT_WRONG_RESOLUTION, MXL_MONITORING_CONTENT_WRONG_RATE})
            {
                if ((mismatch & bit(issue)) != 0)
                {
                    return issue;
                }
            }
            return MXL_MONITORING_CONTENT_WRONG_FORMAT;
        }

        // A v210 row packs six pixels into four 32-bit words of three 10-bit
        // samples, in 128-byte groups of 48 pixels.
        std::uint64_t v210RowBytes(int width)
        {
            return static_cast<std::uint64_t>((width + 47) / 48) * 128;
        }

        // An interlaced grain carries one field, half the frame's rows.
        int v210Rows(mxlMonitoringDeclared const& declared)
        {
            return declared.interlaced ? declared.height / 2 : declared.height;
        }

        // v210 reserves the codes 0-3 and 1020-1023 for timing references, so a
        // picture sample there is not a legal value. Three rows are checked per
        // grain, the active pixels only: a row padded out to 48 pixels carries
        // zeros past the picture. Returns where the first illegal sample sits,
        // or "" when the rows are legal.
        std::string v210RowsIllegal(std::uint8_t const* payload, int width, int rows)
        {
            if (rows <= 0)
            {
                return {};
            }
            auto const rowBytes = v210RowBytes(width);
            auto const activeWords = static_cast<std::uint64_t>(width / 6) * 4;
            for (auto const row : {0, rows / 2, rows - 1})
            {
                auto const* words = reinterpret_cast<std::uint32_t const*>(payload + rowBytes * static_cast<std::uint64_t>(row));
                for (std::uint64_t w = 0; w < activeWords; ++w)
                {
                    auto const word = words[w];
                    for (auto const shift : {0U, 10U, 20U})
                    {
                        auto const sample = (word >> shift) & 0x3ffU;
                        if (sample < 4 || sample > 1019)
                        {
                            return std::format("v210 code {} at row {}, word {}, bit {}", sample, row, w, shift);
                        }
                    }
                }
            }
            return {};
        }

        // Float audio outside -1..1, or not a number at all, across both
        // wrapped fragments of every channel.
        std::string float32Illegal(mxlWrappedMultiBufferSlice const& slices)
        {
            for (std::size_t channel = 0; channel < slices.count; ++channel)
            {
                for (auto const& fragment : slices.base.fragments)
                {
                    if (fragment.pointer == nullptr)
                    {
                        continue;
                    }
                    auto const* base = static_cast<std::uint8_t const*>(fragment.pointer) + channel * slices.stride;
                    auto const* samples = reinterpret_cast<float const*>(base);
                    for (std::size_t i = 0; i < fragment.size / sizeof(float); ++i)
                    {
                        if (!std::isfinite(samples[i]) || std::fabs(samples[i]) > 1.0F)
                        {
                            return std::format("float32 sample {} in channel {}", samples[i], channel);
                        }
                    }
                }
            }
            return {};
        }

        bool found(mxlMonitoringContent what, mxlMonitoringContent* issue)
        {
            if (issue != nullptr)
            {
                *issue = what;
            }
            return true;
        }
    }

    void Flow::recomputeMismatch()
    {
        auto text = std::array<char, MXL_MONITORING_TEXT>{};
        auto bits = std::uint32_t{0};
        if (definitionSet && expectationSet)
        {
            auto expectedCopy = expected;
            expectedCopy.mediaType = expectedMediaType.empty() ? nullptr : expectedMediaType.c_str();
            bits = mxlMonitoringMismatch(&declared, &expectedCopy, text.data(), text.size());
        }
        formatMismatch = text.data();
        mismatch.store(bits, std::memory_order_relaxed);
        checkLegal.store(expectationSet && expected.checkLegal, std::memory_order_relaxed);
    }
}

using namespace mxl::monitoring;

extern "C"
{
    mxlStatus mxlMonitoringParseDefinition(char const* flowDef, mxlMonitoringDeclared* declared)
    {
        if (flowDef == nullptr || declared == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto json = picojson::value{};
        if (!picojson::parse(json, flowDef).empty() || !json.is<picojson::object>())
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const& object = json.get<picojson::object>();
        auto out = mxlMonitoringDeclared{};
        if (auto const it = object.find("media_type"); it != object.end() && it->second.is<std::string>())
        {
            copyText(it->second.get<std::string>(), out.mediaType, sizeof(out.mediaType));
        }
        if (auto const it = object.find("interlace_mode"); it != object.end() && it->second.is<std::string>())
        {
            out.interlaced = it->second.get<std::string>().starts_with("interlaced");
        }
        out.width = jsonInt(object, "frame_width");
        out.height = jsonInt(object, "frame_height");
        out.channels = jsonInt(object, "channel_count");
        if (auto const it = object.find("grain_rate"); it != object.end() && it->second.is<picojson::object>())
        {
            auto const& r = it->second.get<picojson::object>();
            auto const den = jsonInt(r, "denominator");
            out.rate = mxlRational{jsonInt(r, "numerator"), den > 0 ? den : 1};
        }
        if (auto const it = object.find("sample_rate"); it != object.end() && it->second.is<picojson::object>())
        {
            out.sampleRate = jsonInt(it->second.get<picojson::object>(), "numerator");
        }
        *declared = out;
        return MXL_STATUS_OK;
    }

    uint32_t mxlMonitoringMismatch(mxlMonitoringDeclared const* declared, mxlMonitoringExpectation const* expected, char* message,
        size_t messageSize)
    {
        copyText("", message, messageSize);
        if (declared == nullptr || expected == nullptr)
        {
            return 0;
        }
        auto const& d = *declared;
        auto const& e = *expected;
        auto bits = std::uint32_t{0};
        auto parts = std::vector<std::string>{};
        if (e.mediaType != nullptr && *e.mediaType != '\0' && std::string_view{e.mediaType} != d.mediaType)
        {
            bits |= bit(MXL_MONITORING_CONTENT_WRONG_FORMAT);
            parts.push_back(std::format("media type {} where {} was expected", d.mediaType, e.mediaType));
        }
        if ((e.width != 0 && e.width != d.width) || (e.height != 0 && e.height != d.height))
        {
            bits |= bit(MXL_MONITORING_CONTENT_WRONG_RESOLUTION);
            parts.push_back(std::format("{}x{} where {}x{} was expected", d.width, d.height, e.width, e.height));
        }
        if (!unset(e.rate) && (unset(d.rate) || !sameRate(e.rate, d.rate)))
        {
            bits |= bit(MXL_MONITORING_CONTENT_WRONG_RATE);
            parts.push_back(std::format("rate {} where {} was expected", rateText(d.rate), rateText(e.rate)));
        }
        if (e.sampleRate != 0 && e.sampleRate != d.sampleRate)
        {
            bits |= bit(MXL_MONITORING_CONTENT_WRONG_RATE);
            parts.push_back(std::format("sample rate {} where {} was expected", d.sampleRate, e.sampleRate));
        }
        if (e.channels != 0 && e.channels != d.channels)
        {
            bits |= bit(MXL_MONITORING_CONTENT_WRONG_FORMAT);
            parts.push_back(std::format("{} channels where {} were expected", d.channels, e.channels));
        }
        auto joined = std::string{};
        for (auto const& part : parts)
        {
            joined += (joined.empty() ? "" : "; ") + part;
        }
        copyText(joined, message, messageSize);
        return bits;
    }

    bool mxlMonitoringCheckGrain(mxlMonitoringDeclared const* declared, uint32_t mismatch, bool checkLegal, mxlGrainInfo const* grain,
        uint64_t index, uint8_t const* payload, mxlMonitoringContent* issue, char* detail, size_t detailSize)
    {
        copyText("", detail, detailSize);
        if (mismatch != 0)
        {
            return found(mismatchIssue(mismatch), issue);
        }
        if (grain == nullptr)
        {
            return false;
        }
        // libmxl hands back whatever the ring slot holds once the index is
        // within the ring, so a grain whose own index differs is one the
        // writer or the mirror never wrote: the slot still holds an older
        // cycle's grain.
        if (grain->index != index)
        {
            return found(MXL_MONITORING_CONTENT_MISSING, issue);
        }
        if ((grain->flags & MXL_GRAIN_FLAG_INVALID) != 0)
        {
            return found(MXL_MONITORING_CONTENT_INVALID, issue);
        }
        if (grain->validSlices < grain->totalSlices)
        {
            return found(MXL_MONITORING_CONTENT_INCOMPLETE, issue);
        }
        if (declared == nullptr)
        {
            return false;
        }
        auto const v210 = std::string_view{declared->mediaType} == "video/v210" && declared->width > 0 && v210Rows(*declared) > 0;
        if (v210 && grain->grainSize != v210RowBytes(declared->width) * static_cast<std::uint64_t>(v210Rows(*declared)))
        {
            return found(MXL_MONITORING_CONTENT_WRONG_SIZE, issue);
        }
        if (v210 && checkLegal && payload != nullptr)
        {
            if (auto const where = v210RowsIllegal(payload, declared->width, v210Rows(*declared)); !where.empty())
            {
                copyText(where, detail, detailSize);
                return found(MXL_MONITORING_CONTENT_ILLEGAL_VALUES, issue);
            }
        }
        return false;
    }

    bool mxlMonitoringCheckSamples(mxlMonitoringDeclared const* declared, uint32_t mismatch, bool checkLegal,
        mxlWrappedMultiBufferSlice const* samples, mxlMonitoringContent* issue, char* detail, size_t detailSize)
    {
        copyText("", detail, detailSize);
        if (mismatch != 0)
        {
            return found(mismatchIssue(mismatch), issue);
        }
        if (checkLegal && declared != nullptr && samples != nullptr && std::string_view{declared->mediaType} == "audio/float32")
        {
            if (auto const where = float32Illegal(*samples); !where.empty())
            {
                copyText(where, detail, detailSize);
                return found(MXL_MONITORING_CONTENT_ILLEGAL_VALUES, issue);
            }
        }
        return false;
    }

    mxlStatus mxlMonitoringSetExpectation(mxlMonitoringFlow flow, mxlMonitoringExpectation const* expected)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr || expected == nullptr || expected->version != 1 || expected->size < sizeof(mxlMonitoringExpectation))
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const lock = std::lock_guard{f->mutex};
        f->expected = *expected;
        f->expectedMediaType = expected->mediaType != nullptr ? expected->mediaType : "";
        f->expected.mediaType = nullptr;
        f->expectationSet = true;
        f->recomputeMismatch();
        return MXL_STATUS_OK;
    }

    mxlStatus mxlMonitoringSetDefinition(mxlMonitoringFlow flow, char const* flowDef)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto declared = mxlMonitoringDeclared{};
        if (auto const status = mxlMonitoringParseDefinition(flowDef, &declared); status != MXL_STATUS_OK)
        {
            return status;
        }
        auto const lock = std::lock_guard{f->mutex};
        f->declared = declared;
        f->definitionSet = true;
        f->recomputeMismatch();
        return MXL_STATUS_OK;
    }

    mxlStatus mxlMonitoringGetDeclared(mxlMonitoringFlow flow, mxlMonitoringDeclared* declared)
    {
        auto* const f = toFlow(flow);
        if (f == nullptr || declared == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const lock = std::lock_guard{f->mutex};
        if (!f->definitionSet)
        {
            return MXL_ERR_NOT_READY;
        }
        *declared = f->declared;
        return MXL_STATUS_OK;
    }
}
