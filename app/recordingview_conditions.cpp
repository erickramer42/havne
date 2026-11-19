// File/layer: app/recordingview_conditions.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include "recordingview.h"
#include "recordingview_internal.h"

#include "recording_frameset.h"
#include "urlwrite.h"

#include "healthreport.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMetaObject>
#include <QThreadPool>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/profile.h"

#include "recording_signals.h" /* cli/: events.log's profile and file lines */
#include "view_frames.h"       /* cli/: which set decodes the frames */

using namespace recordingview_detail;

namespace {

struct NameAndLevel {
    QString name;
    int level = ECUIO_ALERT_NONE;
};

// Each condition's name and level from the folder's conditions.json,
// through core's loader, while its bytes hash to the header's SHA-256
// (core's MATCH, taken again here on the bytes the loader reads)
std::unordered_map<std::string, NameAndLevel>
namesFromSet(const ecuio_recording_t* reader, const QString& folder)
{
    std::unordered_map<std::string, NameAndLevel> out;
    ecuio_recording_conditions_t c{};
    if (ecuio_recording_get_conditions(reader, &c) != ECUIO_OK) {
        return out;
    }
    QFile f(QDir(folder).filePath(QStringLiteral("conditions.json")));
    if (!f.open(QIODevice::ReadOnly)) {
        return out;
    }
    const QByteArray bytes = f.read(ECUIO_CONDITION_FILE_MAX_BYTES + 1);
    // Absent from the header, changed or past the bound: not the set judged
    if (bytes.size() > static_cast<qsizetype>(ECUIO_CONDITION_FILE_MAX_BYTES) ||
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() !=
            QByteArray(c.sha256)) {
        return out;
    }
    ecuio_condition_set_t* loaded = nullptr;
    if (ecuio_condition_set_load(bytes.constData(),
                                 static_cast<size_t>(bytes.size()),
                                 &loaded) != ECUIO_OK) {
        return out;
    }
    const std::unique_ptr<ecuio_condition_set_t, void (*)(ecuio_condition_set_t*)>
        set(loaded, ecuio_condition_set_free);
    const uint32_t n = ecuio_condition_set_count(set.get());
    for (uint32_t i = 0; i < n; ++i) {
        ecuio_condition_info_t ci{};
        if (ecuio_condition_set_get(set.get(), i, &ci) != ECUIO_OK ||
            ci.key == nullptr) {
            continue;
        }
        NameAndLevel& got = out[ci.key];
        got.name = QString::fromUtf8(ci.name != nullptr ? ci.name : "");
        got.level = static_cast<int>(ci.alert_level); // NONE without alert
    }
    return out;
}

} // namespace

// In events.log's order, which is time order: an entry opens at OFF to ON,
// or at STALE while OFF, and the first ViewerConditionSpans to open are
// kept, so the kept ones are the earliest. The kept spans' stale parts
// share one bound of the same size: past it a span's next part runs to the
// span's end, drawing more stale than there was, never less
void RecordingView::pairConditions(Result& res, const QString& folder,
                                   std::atomic<bool>* cancel)
{
    ecuio_recording_info_t info{};
    if (ecuio_recording_get_info(res.reader, &info) != ECUIO_OK ||
        info.condition_events == 0) {
        return;
    }
    // Core kept the first lines only: nothing after the last is known
    const bool cut = res.cutForTesting ||
                     (info.flags & ECUIO_RECORDING_INFO_CONDITIONS_CUT) != 0;
    struct Track {
        bool seen = false;
        bool on = false;
        bool stale = false;
        bool kept = false;   // the open entry is within the cap
        bool held = false;
        bool merged = false; // past the parts' bound: one part to the end
        uint64_t from = 0;
        uint64_t staleFrom = 0;
        uint64_t mergedFrom = 0;
        std::vector<std::pair<uint64_t, uint64_t>> parts; // an ON span's stale stretches
    };
    std::unordered_map<std::string, uint32_t> index;
    std::vector<Track> tracks;
    const auto cap = static_cast<uint64_t>(ViewerConditionSpans);
    uint64_t partsKept = 0;
    auto open = [&](Track& t) {
        t.kept = res.conditionSpanCount < cap;
        ++res.conditionSpanCount;
    };
    auto emitEntry = [&](uint32_t condition, Track& t, bool on, uint64_t from,
                         uint64_t to, bool heldStart, bool openEnd) {
        if (on && t.merged) {
            t.parts.emplace_back(t.mergedFrom, to);
        }
        t.merged = false;
        auto parts = std::move(t.parts);
        t.parts.clear();
        if (!t.kept) {
            return;
        }
        ConditionSpan s;
        s.condition = condition;
        s.on = on;
        s.heldStart = heldStart;
        s.openEnd = openEnd;
        s.from_us = from;
        s.to_us = to;
        s.stale = on ? std::move(parts)
                     : std::vector<std::pair<uint64_t, uint64_t>>{{from, to}};
        res.conditionSpans.push_back(std::move(s));
    };
    // Ends a stale stretch at `at`: an ON span's part, or an OFF entry
    auto endStale = [&](uint32_t c, Track& t, uint64_t at, bool openEnd) {
        if (!t.on) {
            emitEntry(c, t, false, t.staleFrom, at, false, openEnd);
        } else if (t.kept && !t.merged) {
            if (partsKept < cap) {
                t.parts.emplace_back(t.staleFrom, at);
                ++partsKept;
            } else {
                t.merged = true;
                t.mergedFrom = t.staleFrom;
                res.conditionPartsCut = true;
            }
        }
        t.stale = false;
    };

    try {
        const auto names = namesFromSet(res.reader, folder);
        const uint64_t kept = std::min<uint64_t>(
            info.condition_events, ECUIO_RECORDING_MAX_CONDITION_EVENTS);
        uint64_t last = 0;
        for (uint64_t i = 0; i < kept; ++i) {
            if ((i & 4095u) == 0 && cancel->load()) {
                return;
            }
            ecuio_recording_condition_event_t e{};
            if (ecuio_recording_get_condition_event(res.reader, i, &e) !=
                ECUIO_OK) {
                continue;
            }
            e.key[sizeof e.key - 1] = '\0';
            const auto [it, added] = index.emplace(
                e.key, static_cast<uint32_t>(tracks.size()));
            if (added) {
                tracks.emplace_back();
                Result::ConditionKey k;
                k.key = e.key;
                const auto named = names.find(k.key);
                if (named != names.end()) {
                    k.name = named->second.name;
                    k.level = named->second.level;
                }
                if (k.name.isEmpty()) {
                    k.name = QString::fromUtf8(e.key);
                }
                res.conditionKeys.push_back(std::move(k));
            }
            const uint32_t c = it->second;
            Track& t = tracks[c];
            const bool on = e.state == ECUIO_CONDITION_ON;
            const bool stale = (e.flags & ECUIO_CONDITION_STALE) != 0;
            // A held line stands for the state the recording began in
            const bool held =
                !t.seen && (e.flags & ECUIO_CONDITION_HELD) != 0;
            const uint64_t at = held ? 0 : e.host_us;
            last = std::max(last, e.host_us);
            t.seen = true;
            if (t.stale && (!stale || on != t.on)) {
                endStale(c, t, at, false);
            }
            if (t.on && !on) {
                emitEntry(c, t, true, t.from, at, t.held, false);
                t.on = false;
            }
            if (!t.on && on) {
                t.on = true;
                t.held = held;
                t.from = at;
                open(t);
            }
            if (stale && !t.stale) {
                t.stale = true;
                t.staleFrom = at;
                if (!t.on) {
                    open(t);
                }
            }
        }
        // Still open: to the recording's end, or after a cut to the last
        // line kept, since what came after it isn't known
        const uint64_t end = cut ? last : info.duration_us;
        for (uint32_t c = 0; c < tracks.size(); ++c) {
            Track& t = tracks[c];
            if (t.stale) {
                endStale(c, t, end, !cut);
            }
            if (t.on) {
                emitEntry(c, t, true, t.from, end, t.held, !cut);
            }
        }
        std::stable_sort(res.conditionSpans.begin(), res.conditionSpans.end(),
                         [&res](const ConditionSpan& a, const ConditionSpan& b) {
                             if (a.from_us != b.from_us) {
                                 return a.from_us < b.from_us;
                             }
                             return res.conditionKeys[a.condition].key <
                                    res.conditionKeys[b.condition].key;
                         });
    } catch (const std::bad_alloc&) {
        // No memory for the entries or the names: the viewer shows none
        res.conditionSpans.clear();
        res.conditionSpanCount = 0;
        res.conditionPartsCut = false;
    }
}

RecordingView::RecordingView(QObject* parent) : QObject(parent) {}

RecordingView::~RecordingView()
{
    m_reportStop = true;
    stop();
    // Its end, posted to this object, is dropped with it
    if (m_reportWorker.joinable()) {
        m_reportWorker.join();
    }
}
