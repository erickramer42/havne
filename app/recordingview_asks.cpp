// File/layer: app/recordingview_asks.cpp | Qt6 Core, app layer, C++17
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

bool RecordingView::haveAsks()
{
    if (!m_asks) {
        try {
            m_asks = std::make_shared<Asks>();
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
    return true;
}

bool RecordingView::startAsks()
{
    {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->holdMs = m_asksHoldMs;
        m_asks->yieldHoldMs = m_yieldHoldMs;
        m_asks->loseMeans = m_loseMeansForTesting;
        if (m_asks->answering) {
            return true; // it takes the ask before it ends
        }
        m_asks->answering = true;
    }
    try {
        if (m_failThreadForTesting) {
            throw std::bad_alloc();
        }
        // A task of the global pool with its share of the reader and the
        // asks: a close leaves it to finish, as a stretches pass
        QThreadPool::globalInstance()->start(
            [asks = m_asks, reader = m_readerHold, stop = m_stop,
             view = QPointer<RecordingView>(this), generation = m_generation] {
                answerAsks(asks, reader, stop, view, generation);
            });
    } catch (const std::bad_alloc&) {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->answering = false;
        return false;
    }
    return true;
}

namespace {

// Each id's mean in each of the columns over [t0, t1) of the recording's
// time in microseconds, a row of `columns` per id in out, NaN for none:
// every column from the statistics over its own microseconds. Ends early,
// with ECUIO_ERR_CANCELED, once stale or the open's stopped is set. Starts
// at column `next` (0: out is made) and, with `waiting` set, stops before
// a column after the first with `yielded` and `next` the column it left
// off at, so a call again resumes there
ecuio_status_t ownColumnMeans(ecuio_recording_t* reader,
                              const std::vector<uint32_t>& ids, int64_t t0,
                              int64_t t1, uint32_t columns,
                              const std::atomic<bool>& stale,
                              const std::atomic<bool>& stopped,
                              const std::atomic<bool>& waiting, uint32_t& next,
                              bool& yielded, std::vector<double>& out)
{
    yielded = false;
    if (next == 0) {
        out.assign(ids.size() * columns, NAN);
    }
    std::vector<ecuio_recording_stats_t> stats(ids.size());
    const double step = static_cast<double>(t1 - t0) / columns;
    const uint32_t first = next;
    for (uint32_t c = first; c < columns; ++c) {
        if (stale.load() || stopped.load()) {
            return ECUIO_ERR_CANCELED;
        }
        if (c > first && waiting.load()) {
            next = c;
            yielded = true;
            return ECUIO_OK;
        }
        // The column's first microsecond and the next column's
        const int64_t from = t0 + std::llround(c * step);
        const int64_t past =
            c + 1 == columns ? t1 : t0 + std::llround((c + 1) * step);
        // Before the recording, or too narrow for a closed span
        if (past - 1 <= (std::max<int64_t>)(from, 0)) {
            continue;
        }
        const ecuio_status_t rc = ecuio_recording_stats(
            reader, ids.data(), static_cast<uint32_t>(ids.size()),
            static_cast<uint64_t>((std::max<int64_t>)(from, 0)),
            static_cast<uint64_t>(past - 1), stats.data());
        if (rc != ECUIO_OK) {
            return rc;
        }
        for (size_t k = 0; k < ids.size(); ++k) {
            if (stats[k].samples > 0) {
                out[k * columns + c] = stats[k].mean;
            }
        }
    }
    next = columns;
    return ECUIO_OK;
}

} // namespace

void RecordingView::answerAsks(const std::shared_ptr<Asks>& asks,
                               const std::shared_ptr<ecuio_recording_t>& reader,
                               const std::shared_ptr<Stop>& stop,
                               const QPointer<RecordingView>& view,
                               quint64 generation)
{
    // A closed open's call isn't made: one call is at most a few hundred
    // ms, which only the application's end waits for
    const auto stopped = [&stop] {
        if (!stop->stopped.load()) {
            return false;
        }
        ++stop->readsStopped;
        return true;
    };
    // The means pass under way, kept across turns of the loop: it gives way
    // to a statistics or step ask put in (`waiting`) and resumes at the next
    // column after it, so a cursor ask never waits for a whole pass
    std::optional<MeansAsk> pass;
    std::vector<double> passOut;
    uint32_t passAt = 0;
    for (;;) {
        std::optional<StatsAsk> stats;
        std::optional<StepAsk> step;
        int holdMs = 0;
        int yieldHoldMs = 0;
        bool loseMeans = false;
        {
            // Ending and an ask's arrival are decided under one lock, so an
            // ask put in while this runs is never left behind
            const std::lock_guard<std::mutex> lock(asks->m);
            if (!asks->stats && !asks->step && !asks->means && !pass) {
                asks->answering = false;
                return;
            }
            stats.swap(asks->stats);
            step.swap(asks->step);
            asks->waiting = false;
            bool took = stats || step;
            if (asks->means) {
                // A newer ask replaces a pass under way: its answer is none
                if (pass) {
                    ++asks->stopped;
                }
                pass = std::move(asks->means);
                asks->means.reset();
                passOut.clear();
                passAt = 0;
                asks->meansStale = false;
                took = true;
            }
            holdMs = took ? asks->holdMs : 0;
            yieldHoldMs = asks->yieldHoldMs;
            loseMeans = asks->loseMeans;
        }
        if (holdMs > 0) {
            ++asks->held;
            std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
        }
        // An application ending reads null here, as a stretches pass does
        QCoreApplication* app = QCoreApplication::instance();
        if (app == nullptr) {
            const std::lock_guard<std::mutex> lock(asks->m);
            asks->answering = false;
            return;
        }
        // A copy that can't be allocated leaves its ask unanswered: an
        // exception out of this task would end the process
        try {
            if (step && !stopped()) {
                uint64_t at = ECUIO_RECORDING_NONE;
                const ecuio_status_t rc = ecuio_recording_step(
                    reader.get(), step->signal, step->t, step->direction, &at);
                QMetaObject::invokeMethod(
                    app,
                    [view, request = step->request, rc, at, generation] {
                        if (view) {
                            view->stepFinished(request, rc, at, generation);
                        }
                    },
                    Qt::QueuedConnection);
            }
        } catch (const std::bad_alloc&) {
            ++asks->lost;
        }
        try {
            if (stats && !stopped()) {
                std::vector<ecuio_recording_stats_t> got(stats->ids.size());
                const ecuio_status_t rc = ecuio_recording_stats(
                    reader.get(), stats->ids.data(),
                    static_cast<uint32_t>(stats->ids.size()), stats->t0,
                    stats->t1, got.data());
                QMetaObject::invokeMethod(
                    app,
                    [view, ask = *stats, rc, got, generation] {
                        if (view) {
                            view->statsFinished(ask, rc, got, generation);
                        }
                    },
                    Qt::QueuedConnection);
            }
        } catch (const std::bad_alloc&) {
            ++asks->lost;
        }
        try {
            if (pass) {
                if (loseMeans) {
                    throw std::bad_alloc();
                }
                bool yielded = false;
                const ecuio_status_t rc = ownColumnMeans(
                    reader.get(), pass->ids, pass->t0, pass->t1, pass->columns,
                    asks->meansStale, stop->stopped, asks->waiting, passAt,
                    yielded, passOut);
                if (yielded) {
                    ++asks->yields; // the pass stays, resumed after the ask
                    if (yieldHoldMs > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(yieldHoldMs));
                    }
                } else if (rc == ECUIO_ERR_CANCELED) {
                    // A pass a newer means ask or a close ended has no answer
                    ++asks->stopped;
                    if (stop->stopped.load()) {
                        ++stop->readsStopped;
                    }
                    pass.reset();
                } else {
                    QMetaObject::invokeMethod(
                        app,
                        [view, ask = *pass, rc, got = std::move(passOut),
                         generation] {
                            if (view) {
                                view->meansFinished(ask, rc, got, generation);
                            }
                        },
                        Qt::QueuedConnection);
                    pass.reset();
                    passOut.clear();
                }
            }
        } catch (const std::bad_alloc&) {
            ++asks->lost;
            // A page pairs two views' means and waits for both: this one
            // says it has none, where that much can still be posted
            const std::optional<MeansAsk> lost = std::move(pass);
            pass.reset();
            passOut.clear();
            try {
                if (lost) {
                    QMetaObject::invokeMethod(
                        app,
                        [view, request = lost->request, generation] {
                            if (view) {
                                view->meansLost(request, generation);
                            }
                        },
                        Qt::QueuedConnection);
                }
            } catch (const std::bad_alloc&) {
                ++asks->lost; // not even that: the ask stays unanswered
            }
        }
    }
}

int RecordingView::stats(const QVariantList& signalIndexes, double t0Ms,
                         double t1Ms)
{
    if (m_reader == nullptr || signalIndexes.isEmpty()) {
        return 0;
    }
    StatsAsk ask;
    for (const QVariant& v : signalIndexes) {
        uint32_t i = 0;
        if (!signalIndex(v, &i)) {
            return 0;
        }
        ask.ids.push_back(i);
    }
    ask.t0 = fromMs(t0Ms);
    ask.t1 = fromMs(t1Ms);
    if (ask.t1 <= ask.t0) {
        return 0;
    }
    if (!haveAsks()) {
        return 0;
    }
    ask.request = ++m_lastAsk;
    m_lastStats = ask.request;
    {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->stats = std::move(ask);
        m_asks->waiting = true;
    }
    if (!startAsks()) {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->stats.reset();
        m_lastStats = 0;
        return 0;
    }
    return m_lastStats;
}

int RecordingView::columnMeans(const QVariantList& signalIndexes, double t0Ms,
                               double t1Ms, int columns)
{
    if (m_reader == nullptr || signalIndexes.isEmpty() || columns < 1 ||
        columns > kMaxColumns) {
        return 0;
    }
    MeansAsk ask;
    for (const QVariant& v : signalIndexes) {
        uint32_t i = 0;
        if (!signalIndex(v, &i)) {
            return 0;
        }
        ask.ids.push_back(i);
    }
    if (!signedUs(t0Ms, &ask.t0) || !signedUs(t1Ms, &ask.t1) ||
        ask.t1 <= ask.t0 || !haveAsks()) {
        return 0;
    }
    ask.columns = static_cast<uint32_t>(columns);
    ask.request = ++m_lastAsk;
    m_lastMeans = ask.request;
    {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->means = std::move(ask);
        m_asks->meansStale = true; // an older pass under way stops
    }
    if (!startAsks()) {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->means.reset();
        m_lastMeans = 0;
        return 0;
    }
    return m_lastMeans;
}

int RecordingView::step(int signal, double tMs, bool forward)
{
    uint32_t i = 0;
    if (m_reader == nullptr || !signalIndex(QVariant(signal), &i) ||
        !haveAsks()) {
        return 0;
    }
    StepAsk ask;
    ask.request = ++m_lastAsk;
    ask.signal = i;
    ask.t = fromMs(tMs);
    ask.direction =
        forward ? ECUIO_RECORDING_STEP_FORWARD : ECUIO_RECORDING_STEP_BACKWARD;
    m_lastStep = ask.request;
    {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->step = ask;
        m_asks->waiting = true;
    }
    if (!startAsks()) {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->step.reset();
        m_lastStep = 0;
        return 0;
    }
    return m_lastStep;
}

void RecordingView::statsFinished(
    const StatsAsk& ask, ecuio_status_t rc,
    const std::vector<ecuio_recording_stats_t>& got, quint64 generation)
{
    if (generation != m_generation || ask.request != m_lastStats) {
        return;
    }
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    QVariantList out;
    if (rc == ECUIO_OK) {
        const double seconds = static_cast<double>(ask.t1 - ask.t0) / 1e6;
        for (size_t k = 0; k < ask.ids.size(); ++k) {
            const Signal& s = m_signals[ask.ids[k]];
            const ecuio_recording_stats_t& g = got[k];
            // Every conversion is a rising line, so a mean converts as a value
            const ecuio_display_value_t first = shown(s, g.first);
            const double last = shown(s, g.last).value;
            const double change = last - first.value;
            out.append(QVariantMap{
                {QStringLiteral("index"), static_cast<int>(ask.ids[k])},
                {QStringLiteral("samples"), static_cast<double>(g.samples)},
                {QStringLiteral("first"), first.value},
                {QStringLiteral("last"), last},
                {QStringLiteral("lowest"), shown(s, g.lowest).value},
                {QStringLiteral("highest"), shown(s, g.highest).value},
                {QStringLiteral("mean"), shown(s, g.mean).value},
                {QStringLiteral("change"), change},
                {QStringLiteral("rate"), change / seconds},
                {QStringLiteral("lowestMs"), toMsOrNone(g.lowest_us)},
                {QStringLiteral("highestMs"), toMsOrNone(g.highest_us)},
                {QStringLiteral("unit"), QString::fromUtf8(first.unit)},
                {QStringLiteral("decimals"), first.decimal_places},
            });
        }
    }
    emit statsReady(ask.request, out);
}

void RecordingView::meansLost(int request, quint64 generation)
{
    if (generation == m_generation && request == m_lastMeans) {
        emit columnMeansReady(request, {});
    }
}

void RecordingView::meansFinished(const MeansAsk& ask, ecuio_status_t rc,
                                  const std::vector<double>& got,
                                  quint64 generation)
{
    if (generation != m_generation || ask.request != m_lastMeans) {
        return;
    }
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    QVariantList out;
    const double none = std::numeric_limits<double>::quiet_NaN();
    for (size_t k = 0; rc == ECUIO_OK && k < ask.ids.size(); ++k) {
        const Signal& s = m_signals[ask.ids[k]];
        const bool text = (s.pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
        QList<double> means(static_cast<qsizetype>(ask.columns), none);
        for (uint32_t c = 0; !text && c < ask.columns; ++c) {
            const double mean = got[k * ask.columns + c];
            if (std::isfinite(mean)) {
                // Every conversion is a rising line: a mean converts as a value
                means[static_cast<qsizetype>(c)] = shown(s, mean).value;
            }
        }
        out.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(ask.ids[k])},
            {QStringLiteral("means"), QVariant::fromValue(means)},
        });
    }
    emit columnMeansReady(ask.request, out);
}

void RecordingView::stepFinished(int request, ecuio_status_t rc, uint64_t at,
                                 quint64 generation)
{
    if (generation != m_generation || request != m_lastStep) {
        return;
    }
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    emit stepReady(request, rc == ECUIO_OK ? toMsOrNone(at) : -1.0);
}

double RecordingView::nearestSampleMs(int signal, double tMs)
{
    uint32_t i = 0;
    if (m_reader == nullptr || !signalIndex(QVariant(signal), &i)) {
        return -1.0;
    }
    const Signal& s = m_signals[i];
    if (s.pub.first_us == ECUIO_RECORDING_NONE) {
        return -1.0;
    }
    const uint64_t t = fromMs(tMs);
    if (t <= s.pub.first_us) {
        return toMs(s.pub.first_us);
    }
    if (t >= s.pub.last_us) {
        return toMs(s.pub.last_us);
    }
    char text[kTextCapacity];
    // The sample at or before t (the reader's "in force")
    const auto sampleAtOrBefore = [&](uint64_t when, uint64_t* at) {
        const ecuio_status_t rc =
            ecuio_recording_text_at(m_reader, i, when, text, sizeof(text), at);
        if (rc == ECUIO_ERR_RECORDING_CHANGED) {
            markStale();
        }
        return rc == ECUIO_OK;
    };
    uint64_t before = ECUIO_RECORDING_NONE;
    if (!sampleAtOrBefore(t, &before) || before == ECUIO_RECORDING_NONE) {
        return -1.0;
    }
    // The first sample after t, looked for only as far past t as the one
    // before it is behind: the reader answers "at or before", so each
    // look past t that finds a sample there looks again just short of it
    constexpr int kMaxLooks = 16;
    uint64_t after = ECUIO_RECORDING_NONE;
    uint64_t reach = t + (t - before);
    for (int look = 0; look < kMaxLooks && reach > t; ++look) {
        uint64_t at = ECUIO_RECORDING_NONE;
        if (!sampleAtOrBefore(reach, &at)) {
            return -1.0;
        }
        if (at == ECUIO_RECORDING_NONE || at <= t) {
            break;
        }
        after = at;
        reach = at - 1;
    }
    return toMs(after != ECUIO_RECORDING_NONE && after - t < t - before
                    ? after
                    : before);
}
