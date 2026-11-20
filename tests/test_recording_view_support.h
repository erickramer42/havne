// File/layer: tests/test_recording_view_support.h | GTest + QtCore, app-layer tests
// Testing: BRIDGE: the helpers of test_recording_view*.cpp (test_bridge)

#pragma once

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "recordingview.h"
#include "urlwrite.h"

#include "ecuio/conditions.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

namespace {

const QString kFixtures = QStringLiteral(ECUIO_RECORDING_FIXTURES);

inline QString fixture(const char* name)
{
    return kFixtures + QLatin1Char('/') + QLatin1String(name);
}

// Runs the GUI thread's events until done() or ms pass; done()'s answer
inline bool waitFor(const std::function<bool()>& done, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
}

inline bool settled(const RecordingView& v)
{
    return v.state() != RecordingView::Opening;
}

inline QVariantMap signalNamed(const RecordingView& v, const char* name)
{
    for (const QVariant& s : v.signalList()) {
        const QVariantMap m = s.toMap();
        if (m.value(QStringLiteral("name")).toString() == QLatin1String(name))
            return m;
    }
    return {};
}

// A recording of the car's replies under obd2, frames.csv alone, in a
// folder named as a vehicle's recording is (its label in the name):
// readiness (the light on), one stored code, an engine speed
inline QString carRecording(const QTemporaryDir& root)
{
    const QString folder =
        root.path() + QStringLiteral("/20261107-100000 Daily driver");
    QDir().mkpath(folder);
    QFile events(folder + QStringLiteral("/events.log"));
    if (!events.open(QIODevice::WriteOnly)) {
        return {};
    }
    events.write("ecuio session recording, format 2 (ADR-0049)\n"
                 "started  2026-11-07 10:00:00.000 (local time)\n"
                 "utc  2026-11-07T16:00:00.000Z\n"
                 "core  0.5.0\n"
                 "uri  elm327://COM3?profile=obd2\n"
                 "profile  obd2\n"
                 "frame flags  complete\n"
                 "caller  havne-cli 0.5.0\n"
                 "\n"
                 "10:00:01.000  +1.000  end  havne-cli: seconds: 3 frames, 0 "
                 "values, 0 records dropped, 0 notes dropped\n");
    events.close();
    QFile frames(folder + QStringLiteral("/frames.csv"));
    if (!frames.open(QIODevice::WriteOnly)) {
        return {};
    }
    frames.write("host_us,timestamp_us,id,flags,data\n"
                 "100000,100000,0x7E8,0x0,06 41 01 83 07 E5 00 00\n"
                 "200000,200000,0x7E8,0x0,04 43 01 03 01 00 00 00\n"
                 "300000,300000,0x7E8,0x0,04 41 0C 0C 90 00 00 00\n");
    return folder;
}

// A recording of the signals given as (name, rank), each a number with one
// sample at 0.1 s but those named in quiet, which have none
inline void writeRecording(const QString& folder,
                    const std::vector<std::pair<const char*, int>>& sigs,
                    const QStringList& quiet = {})
{
    QFile events(folder + QStringLiteral("/events.log"));
    ASSERT_TRUE(events.open(QIODevice::WriteOnly));
    QByteArray head = "ecuio session recording, format 2 (ADR-0049)\n"
                      "started  2026-11-03 09:00:00.000 (local time)\n"
                      "profile  test\n";
    for (const auto& s : sigs) {
        head += "signal  " + QByteArray(s.first) + ",number,rpm,0," +
                QByteArray::number(s.second) + "\n";
    }
    head += "\n00:00:01.000  +1.000  end  test: 0 frames, 1 values, 0 records "
            "dropped, 0 notes dropped\n";
    events.write(head);
    QFile rows(folder + QStringLiteral("/signals.csv"));
    ASSERT_TRUE(rows.open(QIODevice::WriteOnly));
    rows.write("host_us,signal,value,unit\n");
    for (const auto& s : sigs) {
        if (quiet.contains(QLatin1String(s.first)))
            continue;
        rows.write("100000," + QByteArray(s.first) + ",1000,rpm\n");
    }
}

} // namespace

// logger-a's CSV imported by its column map into dir's "rec": the folder,
// or "" (the case failed) when the fixture or the import fails
inline QString importedCsv(const QTemporaryDir& dir)
{
    const QString base = QStringLiteral(ECUIO_IMPORT_FIXTURES "/csv/logger-a");
    QFile mapFile(base + QStringLiteral("/map.json"));
    if (!mapFile.open(QIODevice::ReadOnly)) {
        ADD_FAILURE() << "no map.json";
        return QString();
    }
    const QByteArray map = mapFile.readAll();
    const QByteArray in = (base + QStringLiteral("/log.csv")).toUtf8();
    const QByteArray out = (dir.path() + QStringLiteral("/rec")).toUtf8();
    ecuio_import_options_t o{};
    o.format = ECUIO_IMPORT_CSV;
    o.in_path_utf8 = in.constData();
    o.out_folder_utf8 = out.constData();
    o.column_map_utf8 = map.constData();
    ecuio_import_report_t r{};
    if (ecuio_import(&o, &r) != ECUIO_OK) {
        ADD_FAILURE() << ecuio_last_error_detail();
        return QString();
    }
    return QString::fromUtf8(out);
}

// The answer to one column means ask, waited for on the GUI thread: each
// signal's means, in the ask's order
inline std::vector<QList<double>> askedMeans(RecordingView& v, const QVariantList& ids,
                                      double t0, double t1, int columns,
                                      int waitMs = 10000)
{
    QVariantList got;
    bool answered = false;
    const auto c = QObject::connect(&v, &RecordingView::columnMeansReady,
                                    [&](int, const QVariantList& m) {
                                        got = m;
                                        answered = true;
                                    });
    EXPECT_GT(v.columnMeans(ids, t0, t1, columns), 0);
    EXPECT_TRUE(waitFor([&] { return answered; }, waitMs));
    QObject::disconnect(c);
    std::vector<QList<double>> out;
    for (qsizetype k = 0; k < got.size(); ++k) {
        const QVariantMap m = got[k].toMap();
        EXPECT_EQ(m.value(QStringLiteral("index")).toInt(), ids[k].toInt());
        out.push_back(m.value(QStringLiteral("means")).value<QList<double>>());
    }
    return out;
}

// The answer to one stretches ask, waited for on the GUI thread
inline QVariantList askedStretches(RecordingView& v, bool pulls, bool cruises,
                            const QVariantMap& rules = {}, int waitMs = 10000)
{
    QVariantList got;
    bool answered = false;
    const auto c = QObject::connect(&v, &RecordingView::stretchesReady,
                                    [&](int, const QVariantList& s, bool ok) {
                                        got = s;
                                        answered = true;
                                        EXPECT_TRUE(ok);
                                    });
    EXPECT_GT(v.stretches(pulls, cruises, rules), 0)
        << v.stretchesProblem().toStdString();
    EXPECT_TRUE(waitFor([&] { return answered; }, waitMs));
    QObject::disconnect(c);
    return got;
}

// The pulls and cruises one ask finds under rules
inline qsizetype found(RecordingView& v, bool pulls, bool cruises,
                const QVariantMap& rules)
{
    return askedStretches(v, pulls, cruises, rules).size();
}

namespace {

// pull-twice's events.log over a long signals.csv: speed, engine speed
// and throttle every stepMs, a ten-second pull every periodS seconds for
// 6000 s. Every 120 s at 10 ms (the default) is 1.8 million rows and 50
// pulls, fewer than the viewer's first ask holds, so one call is a whole
// pass; every 60 s is 100, which takes a second call
inline QString denseRecording(const QTemporaryDir& dir, int periodS = 120,
                       int stepMs = 10)
{
    const QString folder =
        dir.filePath(QStringLiteral("dense-%1-%2").arg(periodS).arg(stepMs));
    QDir().mkpath(folder);
    QFile::copy(fixture("pull-twice") + QStringLiteral("/events.log"),
                folder + QStringLiteral("/events.log"));
    QFile f(folder + QStringLiteral("/signals.csv"));
    if (!f.open(QIODevice::WriteOnly)) {
        return {};
    }
    f.write("host_us,signal,value,unit\n");
    QByteArray chunk;
    const qint64 period = periodS * qint64{1000000};
    for (qint64 us = 0; us < 6000 * qint64{1000000}; us += stepMs * 1000) {
        const bool pull = us % period >= period - 10000000;
        const qint64 into = us % period - (period - 10000000);
        chunk += QByteArray::number(us) + ",Vehicle Speed," +
                 QByteArray::number(pull ? 60 + into / 500000 : 60) +
                 ",km/h\n" + QByteArray::number(us + 1) + ",RPM," +
                 QByteArray::number(pull ? 2500 + into / 2500 : 2500) +
                 ",rpm\n" + QByteArray::number(us + 2) + ",Throttle," +
                 (pull ? "100.0" : "12.0") + ",%\n";
        if (chunk.size() > (1 << 20)) {
            f.write(chunk);
            chunk.clear();
        }
    }
    f.write(chunk);
    return folder;
}

// The entries of an open recording's conditionSpans, as maps
inline QList<QVariantMap> spansOf(const RecordingView& v)
{
    QList<QVariantMap> out;
    for (const QVariant& s : v.conditionSpans()) {
        out << s.toMap();
    }
    return out;
}

// A stale list's stretches as [from, to] pairs, in ms
inline QList<std::pair<double, double>> staleOf(const QVariantMap& span)
{
    QList<std::pair<double, double>> out;
    for (const QVariant& s : span.value(QStringLiteral("stale")).toList()) {
        const QVariantMap m = s.toMap();
        out << std::make_pair(m.value(QStringLiteral("fromMs")).toDouble(),
                              m.value(QStringLiteral("toMs")).toDouble());
    }
    return out;
}

} // namespace
