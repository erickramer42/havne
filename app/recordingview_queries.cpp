// File/layer: app/recordingview_queries.cpp | Qt6 Core, app layer, C++17
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

void RecordingView::markStale()
{
    if (m_stale) {
        return;
    }
    m_stale = true;
    emit staleChanged();
}

QVariantList RecordingView::columns(const QVariantList& signalIndexes,
                                    double t0Ms, double t1Ms, int columns)
{
    if (m_reader == nullptr || signalIndexes.isEmpty() || columns < 1 ||
        columns > kMaxColumns) {
        return {};
    }
    std::vector<uint32_t> ids;
    for (const QVariant& v : signalIndexes) {
        uint32_t i = 0;
        if (!signalIndex(v, &i)) {
            return {};
        }
        ids.push_back(i);
    }
    const size_t n = static_cast<size_t>(columns);
    std::vector<ecuio_recording_column_t> cols(ids.size() * n);
    if (!query(ids, t0Ms, t1Ms, static_cast<uint32_t>(n), cols)) {
        return {};
    }

    const double none = std::numeric_limits<double>::quiet_NaN();
    QVariantList out;
    for (size_t k = 0; k < ids.size(); ++k) {
        const Signal& s = m_signals[ids[k]];
        const bool text = (s.pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
        QList<double> lows(columns, none);
        QList<double> highs(columns, none);
        QList<double> samples(columns, 0.0);
        for (size_t c = 0; c < n; ++c) {
            const ecuio_recording_column_t& col = cols[k * n + c];
            samples[static_cast<qsizetype>(c)] = col.samples;
            if (col.samples == 0 || text) {
                continue;
            }
            lows[static_cast<qsizetype>(c)] = shown(s, col.lowest).value;
            highs[static_cast<qsizetype>(c)] = shown(s, col.highest).value;
        }
        out.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(ids[k])},
            {QStringLiteral("lows"), QVariant::fromValue(lows)},
            {QStringLiteral("highs"), QVariant::fromValue(highs)},
            {QStringLiteral("samples"), QVariant::fromValue(samples)},
        });
    }
    return out;
}

// The reader takes no time before the start, so a window from before it
// is split as havne-cli view splits one (cli/view_lines.h, window_of):
// the columns wholly before the start stay empty, the one across it is a
// query of its own from 0, and the rest another. Columns of a second or
// more place whole seconds, each in the column it starts in, the first
// column taking one that starts before it: there the column across the
// start ends at its last whole second, and the second across the split
// goes to the next column, so none is counted twice.
bool RecordingView::query(const std::vector<uint32_t>& ids, double t0Ms,
                          double t1Ms, uint32_t n,
                          std::vector<ecuio_recording_column_t>& cols)
{
    int64_t t0 = 0;
    int64_t t1 = 0;
    if (!signedUs(t0Ms, &t0) || !signedUs(t1Ms, &t1) || t1 <= t0) {
        return false;
    }
    const auto nsig = static_cast<uint32_t>(ids.size());
    std::vector<ecuio_recording_column_t> got;
    // count columns over [from, to) into columns [first, first + count)
    const auto into = [&](uint64_t from, uint64_t to, uint32_t first,
                          uint32_t count) {
        got.assign(static_cast<size_t>(nsig) * count, {});
        const ecuio_status_t rc = ecuio_recording_query(
            m_reader, ids.data(), nsig, from, to, got.data(), count);
        if (rc == ECUIO_ERR_RECORDING_CHANGED) {
            markStale();
        }
        if (rc != ECUIO_OK) {
            return false;
        }
        for (size_t k = 0; k < nsig; ++k) {
            std::copy(
                got.begin() + static_cast<std::ptrdiff_t>(k * count),
                got.begin() + static_cast<std::ptrdiff_t>((k + 1) * count),
                cols.begin() + static_cast<std::ptrdiff_t>(k * n + first));
        }
        return true;
    };
    if (t0 >= 0) {
        return into(static_cast<uint64_t>(t0), static_cast<uint64_t>(t1), 0, n);
    }
    if (t1 <= 0) {
        return true; // all before the start
    }
    const int64_t width = (t1 - t0 + n - 1) / n;
    // The first column starting at the start or later: t0 + n x width
    // reaches t1, past 0, so it's within the n
    const auto skip = static_cast<uint32_t>((-t0 + width - 1) / width);
    const uint32_t rest = n - skip;
    const auto restT0 = static_cast<uint64_t>(t0 + skip * width);
    if (skip > 0 && restT0 > 0) {
        uint64_t end = restT0;
        if (rest > 0 && width >= 1000000) {
            end = restT0 / 1000000 * 1000000;
        }
        if (end > 0 && !into(0, end, skip - 1, 1)) {
            return false;
        }
    }
    return rest == 0 ||
           into(restT0, static_cast<uint64_t>(t0 + n * width), skip, rest);
}

double RecordingView::found(const ecuio_recording_find_t& what)
{
    uint64_t at = ECUIO_RECORDING_NONE;
    const ecuio_status_t rc = ecuio_recording_find(m_reader, &what, 0, &at);
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    return rc == ECUIO_OK ? toMsOrNone(at) : -1.0;
}

double RecordingView::findThreshold(int signal, double level, double hysteresis,
                                    bool rising)
{
    uint32_t i = 0;
    if (m_reader == nullptr || !signalIndex(signal, &i) ||
        (m_signals[i].pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0 ||
        !std::isfinite(level) || !(hysteresis >= 0)) {
        return -1.0;
    }
    // The level as recorded: every display conversion is a rising line
    // (signalList's ranges rest on it too), so two points of it invert it
    const Signal& s = m_signals[i];
    const double at0 = shown(s, 0).value;
    const double slope = shown(s, 1).value - at0;
    if (!(slope > 0)) {
        return -1.0;
    }
    ecuio_recording_find_t what{};
    what.kind = ECUIO_RECORDING_FIND_THRESHOLD;
    what.signal = i;
    what.direction =
        rising ? ECUIO_RECORDING_FIND_RISING : ECUIO_RECORDING_FIND_FALLING;
    what.level = (level - at0) / slope;
    what.hysteresis = hysteresis / slope;
    return found(what);
}

double RecordingView::findMarker(const QString& label)
{
    if (m_reader == nullptr) {
        return -1.0;
    }
    const QByteArray utf8 = label.toUtf8();
    ecuio_recording_find_t what{};
    what.kind = ECUIO_RECORDING_FIND_MARKER;
    what.label_utf8 = utf8.constData();
    return found(what);
}

double RecordingView::findEngineStart()
{
    if (m_reader == nullptr) {
        return -1.0;
    }
    ecuio_recording_find_t what{};
    what.kind = ECUIO_RECORDING_FIND_ENGINE_START;
    return found(what);
}

QVariantList RecordingView::valuesAt(const QVariantList& signalIndexes,
                                     double tMs)
{
    if (m_reader == nullptr) {
        return {};
    }
    const uint64_t t = fromMs(tMs);
    QVariantList out;
    char text[kTextCapacity];
    for (const QVariant& v : signalIndexes) {
        uint32_t i = 0;
        if (!signalIndex(v, &i)) {
            return {};
        }
        const Signal& s = m_signals[i];
        uint64_t at = ECUIO_RECORDING_NONE;
        const ecuio_status_t rc =
            ecuio_recording_text_at(m_reader, i, t, text, sizeof(text), &at);
        if (rc == ECUIO_ERR_RECORDING_CHANGED) {
            markStale();
        }
        if (rc != ECUIO_OK) {
            return {};
        }
        QString shownText = QString::fromUtf8(text);
        QString unit = QString::fromUtf8(s.pub.unit);
        QString label;
        const bool number = (s.pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) == 0;
        if (number && at != ECUIO_RECORDING_NONE) {
            // signals.csv's numbers are C-locale, as QByteArray reads them
            bool ok = false;
            const double value = QByteArray(text).toDouble(&ok);
            if (ok) {
                const ecuio_display_value_t d = shown(s, value);
                shownText = QString::number(d.value, 'f', d.decimal_places);
                unit = QString::fromUtf8(d.unit);
                // An enum's raw number and its label (format 3's
                // events.log): the word shows, the number plots
                if (value >= 0.0 && value <= 4294967295.0 &&
                    value == std::floor(value)) {
                    const char* l = ecuio_recording_enum_label(
                        m_reader, i, static_cast<uint32_t>(value));
                    if (l != nullptr) {
                        label = QString::fromUtf8(l);
                    }
                }
            }
        } else if (number) {
            unit = QString::fromUtf8(shown(s, 0).unit);
        }
        out.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(i)},
            {QStringLiteral("text"), shownText},
            {QStringLiteral("unit"), unit},
            {QStringLiteral("label"), label},
            {QStringLiteral("atMs"), toMsOrNone(at)},
        });
    }
    return out;
}
