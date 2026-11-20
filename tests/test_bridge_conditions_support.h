// File/layer: tests/test_bridge_conditions_support.h | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: the helpers of test_bridge_conditions*.cpp (test_bridge)

#pragma once

#include <cmath>
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <thread>
#include <map>
#include <vector>

#include "sessionbridge.h"   // SessionBridge + FrameListModel

#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's seams: a failed write, the attach's handover
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES
#include "bridge_support.h"

// ---------------------------------------------------------------------------
// The conditions (ADR-0063 section 9): the app's saved set
// (app/conditionstore.h), its rows (app/conditionmodel.h) and the bridge's
// attach, callback and tick. The mock's first 15 s are its KeyOn (0 rpm),
// so a condition true there fires at the first frame and stays ON.
// ---------------------------------------------------------------------------

namespace {

// One condition, so a set told from mockConditions by its count; of two
// terms, on two units
inline QByteArray otherConditions()
{
    return QByteArray(R"({"havne_conditions":1,"conditions":[)"
                      R"({"key":"second","name":"Second set","when":{"all":[)"
                      R"({"signal":["id:1001"],"unit":"RPM","below":300},)"
                      R"({"signal":["id:3001"],"unit":"km/h","below":250}]},)"
                      R"("alert":{"level":"warning"}}]})");
}

// The model's rows, each a map by role name
inline QVariantList conditionRows(const ConditionModel* model)
{
    QVariantList rows;
    const QHash<int, QByteArray> roles = model->roleNames();
    for (int r = 0; r < model->rowCount(); ++r) {
        QVariantMap row;
        for (auto it = roles.begin(); it != roles.end(); ++it) {
            row.insert(QString::fromLatin1(it.value()),
                       model->data(model->index(r), it.key()));
        }
        rows << row;
    }
    return rows;
}

inline QVariantMap conditionRow(const ConditionModel* model, int r)
{
    return conditionRows(model).value(r).toMap();
}

// Whether row r reads live and in that state, within 5 s
inline bool rowReads(const SessionBridge& bridge, int r, const char* state)
{
    return spinUntil(
        [&]() {
            const QVariantMap row = conditionRow(bridge.conditionModel(), r);
            return row.value(QStringLiteral("live")).toBool() &&
                   row.value(QStringLiteral("state")).toString() ==
                       QLatin1String(state);
        },
        5000);
}

// An event as core's worker gives it
inline ecuio_condition_event_t conditionEvent(uint32_t condition, uint32_t generation,
                                       uint32_t flags, double value,
                                       uint64_t at_us = 1000000,
                                       uint32_t term = 0)
{
    ecuio_condition_event_t e{};
    e.timestamp_us = at_us;
    e.term = term;
    e.condition = condition;
    e.state = (flags & ECUIO_CONDITION_FIRED) != 0 ? ECUIO_CONDITION_ON
                                                   : ECUIO_CONDITION_OFF;
    e.flags = flags;
    e.value = value;
    e.generation = generation;
    return e;
}

// The lines of log starting with prefix
inline QStringList linesStarting(const QStringList& log, const QString& prefix)
{
    QStringList lines;
    for (const QString& line : log) {
        if (line.startsWith(prefix))
            lines << line;
    }
    return lines;
}

// Three alerts on an engine speed the mock never reaches, so only the
// events a case dispatches fire: two that sound, at each level, and one
// that doesn't
inline QByteArray toneConditions()
{
    return QByteArray(
        R"({"havne_conditions":1,"conditions":[)"
        R"({"key":"hot","name":"Hot","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":20000},)"
        R"("alert":{"level":"danger","sound":true}},)"
        R"({"key":"warm","name":"Warm","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":19000},)"
        R"("alert":{"level":"warning","sound":true}},)"
        R"({"key":"quiet","name":"Quiet","when":{"signal":["id:1001"],)"
        R"("unit":"RPM","above":18000},)"
        R"("alert":{"level":"danger","banner":true}}]})");
}

// A session of mock://demo judging toneConditions, its log kept
struct ToneRun {
    SettingsDirScope dir;
    SessionBridge bridge;
    QStringList log;
    uint32_t generation = 0;

    bool start()
    {
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [this](const QString& line) { log << line; });
        if (!dir.valid() ||
            !bridge.saveConditions(QString::fromUtf8(toneConditions()))
                 .value(QStringLiteral("ok"))
                 .toBool() ||
            !bridge.connectSession(QStringLiteral("mock://demo")).isEmpty() ||
            !rowReads(bridge, 0, "off")) {
            return false;
        }
        generation = bridge.conditionsGenerationForTesting();
        return true;
    }
    // Each condition fired in one window, then the tick that writes it
    bool fire(std::initializer_list<uint32_t> conditions)
    {
        std::vector<ecuio_condition_event_t> events;
        for (uint32_t c : conditions) {
            events.push_back(
                conditionEvent(c, generation, ECUIO_CONDITION_FIRED, 20001));
        }
        const qsizetype before =
            linesStarting(log, QStringLiteral("Alert: ")).size();
        bridge.dispatchConditionsForTesting(
            events.data(), static_cast<uint32_t>(events.size()));
        const auto want = before + static_cast<qsizetype>(events.size());
        return spinUntil(
            [&]() {
                return linesStarting(log, QStringLiteral("Alert: ")).size() >=
                       want;
            },
            5000);
    }
    uint64_t danger() const
    {
        return bridge.alertToneForTesting().playedForTesting(
            ECUIO_ALERT_DANGER);
    }
    uint64_t warning() const
    {
        return bridge.alertToneForTesting().playedForTesting(
            ECUIO_ALERT_WARNING);
    }
};

// A tone maker standing in for Android's: each call recorded in a state
// the case keeps after the player is gone
struct FakeTones : AlertTone::Backend {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<std::pair<uint32_t, std::chrono::steady_clock::time_point>>
            starts;
        int makes = 0;
        int releases = 0;
        std::vector<std::chrono::steady_clock::time_point> makeTimes;
        std::vector<std::chrono::steady_clock::time_point> releaseTimes;
        int failMakes = 0;    // the next n makes fail
        int failStarts = 0;   // the next n starts fail
        int blockFirstMs = 0; // the first start takes this long
    };
    std::shared_ptr<State> s = std::make_shared<State>();

    bool make() override
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        ++s->makes;
        s->makeTimes.push_back(std::chrono::steady_clock::now());
        s->changed.notify_all();
        if (s->failMakes > 0) {
            --s->failMakes;
            return false;
        }
        return true;
    }
    bool start(uint32_t level) override
    {
        int block = 0;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            std::swap(block, s->blockFirstMs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(block));
        std::lock_guard<std::mutex> lock(s->mutex);
        s->changed.notify_all();
        if (s->failStarts > 0) {
            --s->failStarts;
            return false;
        }
        s->starts.emplace_back(level, std::chrono::steady_clock::now());
        return true;
    }
    void release() override
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        ++s->releases;
        s->releaseTimes.push_back(std::chrono::steady_clock::now());
        s->changed.notify_all();
    }
};

// Waits up to ms for pred over the state, under its lock
template <typename Pred> bool toneState(FakeTones::State& s, int ms, Pred pred)
{
    std::unique_lock<std::mutex> lock(s.mutex);
    return s.changed.wait_for(lock, std::chrono::milliseconds(ms),
                              [&]() { return pred(s); });
}

} // namespace
