// File/layer: app/recordingview_stretches.cpp | Qt6 Core, app layer, C++17
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

enum class RuleUnit { Percent, Seconds, Rpm, Speed };

// One level of ecuio_stretch_rules_t as a page sets it: its key in the
// rules map, its name in a refusal's words, core's default (%, seconds,
// rpm or km/h) and where a value in core's unit (%, us, rpm, km/h) goes
struct StretchRule {
    const char* key;
    const char* name;
    RuleUnit unit;
    double fallback;
    void (*set)(ecuio_stretch_rules_t&, double);
};

uint64_t wholeUs(double us)
{
    return static_cast<uint64_t>(std::llround(us));
}

const StretchRule kStretchRules[] = {
    {"pullLevel", "Pull throttle", RuleUnit::Percent,
     ECUIO_STRETCH_DEFAULT_PULL_LEVEL,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_level = v; }},
    {"pullLoadLevel", "Pull load, with no throttle", RuleUnit::Percent,
     ECUIO_STRETCH_DEFAULT_PULL_LOAD_LEVEL,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_load_level = v; }},
    {"pullHoldS", "Pull at least", RuleUnit::Seconds,
     ECUIO_STRETCH_DEFAULT_PULL_HOLD_US / 1e6,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_hold_us = wholeUs(v); }},
    {"pullGapS", "A dip shorter than", RuleUnit::Seconds,
     ECUIO_STRETCH_DEFAULT_PULL_GAP_US / 1e6,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_gap_us = wholeUs(v); }},
    {"pullRise", "Engine speed rises", RuleUnit::Rpm, ECUIO_STRETCH_DEFAULT_PULL_RISE,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_min_rise = v; }},
    {"pullGain", "Speed gains, no rpm", RuleUnit::Speed, ECUIO_STRETCH_DEFAULT_PULL_GAIN,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_min_gain = v; }},
    {"cruiseHoldS", "Cruise at least", RuleUnit::Seconds,
     ECUIO_STRETCH_DEFAULT_CRUISE_HOLD_US / 1e6,
     [](ecuio_stretch_rules_t& r, double v) { r.cruise_hold_us = wholeUs(v); }},
    {"cruiseSpeedBand", "Speed within", RuleUnit::Speed,
     ECUIO_STRETCH_DEFAULT_CRUISE_SPEED_BAND,
     [](ecuio_stretch_rules_t& r, double v) { r.cruise_speed_band = v; }},
    {"cruiseThrottleBand", "Throttle within", RuleUnit::Percent,
     ECUIO_STRETCH_DEFAULT_CRUISE_THROTTLE_BAND,
     [](ecuio_stretch_rules_t& r, double v) { r.cruise_throttle_band = v; }},
    {"cruiseFloor", "Cruise above", RuleUnit::Speed,
     ECUIO_STRETCH_DEFAULT_CRUISE_FLOOR,
     [](ecuio_stretch_rules_t& r, double v) { r.cruise_floor = v; }},
};

// The most a rule takes: a percentage, an hour, and a speed in km/h
constexpr double kPercentMost = 100.0;
constexpr double kSecondsMost = 3600.0;
constexpr double kSpeedMostKmh = 1000.0;

// 1 km/h in the unit system's speed unit, by core's display table
double speedPerKmh(uint32_t units)
{
    ecuio_display_value_t one{};
    return ecuio_display_convert("km/h", 0, 1.0, units, &one) ==
                   ECUIO_DISPLAY_OK
               ? one.value
               : 1.0;
}

bool isNumber(const QVariant& v)
{
    switch (v.typeId()) {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    default:
        return false;
    }
}

// Where a pull ends, the one rule that is a word: its key and words
constexpr const char* kPullEndKey = "pullEnd";
constexpr const char* kPullEndLift = "lift";
constexpr const char* kPullEndTopSpeed = "top-speed";

// A page's rules map as core's struct, its speeds from the unit system's
// unit to km/h; false with the refusal's words, naming the rule
bool stretchRulesOf(const QVariantMap& map, uint32_t units,
                    ecuio_stretch_rules_t* out, QString* problem)
{
    const double perKmh = speedPerKmh(units);
    const bool imperial = units == ECUIO_UNITS_IMPERIAL;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (it.key() == QLatin1String(kPullEndKey)) {
            const QString word = it.value().typeId() == QMetaType::QString
                                     ? it.value().toString()
                                     : QString();
            if (word == QLatin1String(kPullEndTopSpeed)) {
                out->flags |= ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED;
            } else if (word == QLatin1String(kPullEndLift)) {
                out->flags &= ~ECUIO_STRETCH_PULL_ENDS_AT_TOP_SPEED;
            } else {
                *problem = QStringLiteral(
                    "Where a pull ends must be lift or top-speed");
                return false;
            }
            continue;
        }
        const StretchRule* rule = nullptr;
        for (const StretchRule& r : kStretchRules) {
            if (it.key() == QLatin1String(r.key)) {
                rule = &r;
            }
        }
        if (rule == nullptr) {
            *problem =
                QStringLiteral("'%1' isn't a stretch rule").arg(it.key());
            return false;
        }
        const double shown = isNumber(it.value()) ? it.value().toDouble() : NAN;
        double value = shown; // in core's unit
        double most = INFINITY;
        QString range = QStringLiteral(", 0 or more");
        switch (rule->unit) {
        case RuleUnit::Percent:
            most = kPercentMost;
            range = QStringLiteral(" from 0 to 100");
            break;
        case RuleUnit::Seconds:
            most = kSecondsMost;
            range = QStringLiteral(" from 0 to 3600 s");
            value = shown * 1e6;
            break;
        case RuleUnit::Speed:
            most = kSpeedMostKmh * perKmh;
            range = QStringLiteral(" from 0 to %1 %2")
                        .arg(static_cast<int>(most))
                        .arg(imperial ? QStringLiteral("mph")
                                      : QStringLiteral("km/h"));
            value = shown / perKmh;
            break;
        case RuleUnit::Rpm:
            break;
        }
        if (!std::isfinite(shown) || shown < 0.0 || shown > most) {
            *problem = QStringLiteral("%1 must be a number%2")
                           .arg(QLatin1String(rule->name), range);
            return false;
        }
        rule->set(*out, value);
    }
    return true;
}

} // namespace

QVariantMap RecordingView::stretchDefaults() const
{
    const double perKmh = speedPerKmh(m_units);
    QVariantMap out;
    for (const StretchRule& r : kStretchRules) {
        out.insert(QLatin1String(r.key), r.unit == RuleUnit::Speed
                                             ? r.fallback * perKmh
                                             : r.fallback);
    }
    out.insert(QLatin1String(kPullEndKey), QLatin1String(kPullEndLift));
    return out;
}

QVariantMap RecordingView::stretchRuleNames() const
{
    QVariantMap out;
    for (const StretchRule& r : kStretchRules) {
        out.insert(QLatin1String(r.key), QString::fromLatin1(r.name));
    }
    return out;
}

void RecordingView::setStretchesProblem(const QString& words)
{
    if (words != m_stretchesProblem) {
        m_stretchesProblem = words;
        emit stretchesProblemChanged();
    }
}

void RecordingView::findStretches(
    const std::shared_ptr<ecuio_recording_t>& reader,
    const std::shared_ptr<Stop>& stop, const QPointer<RecordingView>& view,
    int request, ecuio_stretch_rules_t rules, quint64 generation, int holdMs,
    int stopAt, int holdAt)
{
    struct Asked {
        Stop* stop;
        int request;
        int stopAt; // a test's
        int holdAt; // a test's
        int asks;
    } asked{stop.get(), request, stopAt, holdAt, 0};
    // A close or a newer stretches ask ends the pass at its next second
    const ecuio_cancel_t closed = [](void* user) {
        auto* a = static_cast<Asked*>(user);
        ++a->asks;
        if (a->asks == a->holdAt) {
            ++a->stop->stretchesHeld;
            const auto until =
                std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!a->stop->stopped.load() &&
                   a->stop->latestStretches.load() <= a->request &&
                   std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return a->stop->stopped.load() ||
                       a->stop->latestStretches.load() > a->request ||
                       (a->stopAt > 0 && a->asks >= a->stopAt)
                   ? 1
                   : 0;
    };
    if (holdMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    }
    ecuio_recording_info_t info{};
    ecuio_recording_get_info(reader.get(), &info);
    const uint64_t end = info.duration_us + 1;
    // One pass for a recording with no more stretches than this; a second
    // only for one with more
    const uint32_t first = 64;
    ecuio_status_t rc = ECUIO_ERR_OUT_OF_MEMORY;
    std::vector<ecuio_recording_stretch_t> got;
    try {
        got.resize(first);
        uint32_t count = 0;
        rc =
            ecuio_recording_stretches_with(reader.get(), &rules, 0, end, closed,
                                           &asked, got.data(), first, &count);
        if (rc == ECUIO_OK && count > first) {
            got.resize(count);
            rc = ecuio_recording_stretches_with(reader.get(), &rules, 0, end,
                                                closed, &asked, got.data(),
                                                count, &count);
        }
        got.resize(rc == ECUIO_OK ? (std::min<size_t>)(got.size(), count) : 0);
    } catch (const std::bad_alloc&) {
        got.clear();
        rc = ECUIO_ERR_OUT_OF_MEMORY;
    }
    stop->passAsks = asked.asks;
    if (rc == ECUIO_ERR_CANCELED) {
        // Closed or asked again: its answer would be dropped
        if (!stop->stopped.load() && stop->latestStretches.load() > request) {
            ++stop->stretchesStaled;
        } else {
            ++stop->readsStopped;
        }
        return;
    }
    // An application ending reads null here (Qt 6.12 clears instance(),
    // then waits for the pool); one read non-null lives until this ends
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(
        app,
        [view, request, rc, got, generation] {
            if (view) {
                view->stretchesFinished(request, rc, got, generation);
            }
        },
        Qt::QueuedConnection);
}

int RecordingView::stretches(bool pulls, bool cruises, const QVariantMap& rules)
{
    // Judged here, before any task: a refusal is the caller's at once
    ecuio_stretch_rules_t asked{};
    QString problem;
    const bool taken = stretchRulesOf(rules, m_units, &asked, &problem);
    setStretchesProblem(problem);
    if (!taken || m_reader == nullptr || (!pulls && !cruises)) {
        return 0;
    }
    asked.kinds = (pulls ? ECUIO_STRETCH_PULLS : 0u) |
                  (cruises ? ECUIO_STRETCH_CRUISES : 0u);
    asked.throttle = asked.engine_load = UINT32_MAX;
    asked.vehicle_speed = asked.engine_speed = UINT32_MAX;
    const int request = ++m_lastAsk;
    try {
        if (m_failThreadForTesting) {
            throw std::bad_alloc();
        }
        // A task of the global pool with its share of the reader: a close
        // or a newer stretches ask stops it at its next second, and
        // QCoreApplication's end waits for it
        QThreadPool::globalInstance()->start(
            [reader = m_readerHold, stop = m_stop,
             view = QPointer<RecordingView>(this), request, asked,
             generation = m_generation, holdMs = m_stretchesHoldMs,
             stopAt = m_stretchesStopAt, holdAt = m_stretchesHoldAt] {
                findStretches(reader, stop, view, request, asked, generation,
                              holdMs, stopAt, holdAt);
            });
    } catch (const std::bad_alloc&) {
        return 0;
    }
    // Only a pass that started ends an older one
    m_stop->latestStretches = request;
    m_lastStretches = request;
    return request;
}

void RecordingView::stretchesFinished(
    int request, ecuio_status_t rc,
    const std::vector<ecuio_recording_stretch_t>& got, quint64 generation)
{
    if (generation != m_generation || request != m_lastStretches) {
        return;
    }
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    QVariantList out;
    for (size_t k = 0; rc == ECUIO_OK && k < got.size(); ++k) {
        const ecuio_recording_stretch_t& g = got[k];
        if (g.signal >= m_signals.size()) {
            continue;
        }
        const ecuio_display_value_t start = shown(m_signals[g.signal], g.start);
        out.append(QVariantMap{
            {QStringLiteral("kind"), g.kind == ECUIO_RECORDING_STRETCH_PULL
                                         ? QStringLiteral("pull")
                                         : QStringLiteral("cruise")},
            {QStringLiteral("fromMs"), toMs(g.from_us)},
            {QStringLiteral("toMs"), toMs(g.to_us)},
            {QStringLiteral("start"), start.value},
            {QStringLiteral("end"), shown(m_signals[g.signal], g.end).value},
            {QStringLiteral("unit"), QString::fromUtf8(start.unit)},
            {QStringLiteral("decimals"), start.decimal_places},
            {QStringLiteral("signal"), static_cast<int>(g.signal)},
        });
    }
    emit stretchesReady(request, out, rc == ECUIO_OK);
}
