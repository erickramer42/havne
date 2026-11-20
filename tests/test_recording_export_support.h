// File/layer: tests/test_recording_export_support.h | GoogleTest, Qt6 Core, bridge tier
// Testing: BRIDGE: the helpers of test_recording_export*.cpp (test_bridge)

#pragma once

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <vector>

#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "recordingexport.h"
#include "urlwrite.h"

namespace {

inline bool waitUntil(const std::function<bool()>& done, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
}

inline QString fixture(const char* name)
{
    return QStringLiteral(ECUIO_RECORDING_FIXTURES "/") + QLatin1String(name);
}

inline QByteArray bytesOf(const QString& path)
{
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly)) << path.toStdString();
    return f.readAll();
}

inline void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << path.toStdString();
    f.write(bytes);
}

struct Options {
    int format = RecordingExport::Csv;
    bool longLayout = false;
    quint32 rowMs = 0;
    quint32 units = ECUIO_UNITS_METRIC;
    QStringList columns;
    bool raw = false;
};

inline QVariantMap asMap(const QString& folder, const Options& o)
{
    return {{QStringLiteral("folder"), folder},
            {QStringLiteral("format"), o.format},
            {QStringLiteral("longLayout"), o.longLayout},
            {QStringLiteral("rowMs"), o.rowMs},
            {QStringLiteral("units"), o.units},
            {QStringLiteral("columns"), o.columns},
            {QStringLiteral("raw"), o.raw}};
}

inline QVariantMap exported(RecordingExport& e, const QString& folder,
                     const Options& o, const QUrl& target)
{
    QVariantMap result;
    bool done = false;
    QObject context;
    QObject::connect(&e, &RecordingExport::finished, &context,
                     [&](const QVariantMap& r) {
                         result = r;
                         done = true;
                     });
    EXPECT_TRUE(e.start(asMap(folder, o), target));
    EXPECT_TRUE(e.busy());
    EXPECT_TRUE(waitUntil([&] { return done; })) << "the export ends";
    EXPECT_FALSE(e.busy());
    return result;
}

inline bool ok(const QVariantMap& r)
{
    return r.value(QStringLiteral("ok")).toBool();
}

inline QString text(const QVariantMap& r, const char* key)
{
    return r.value(QLatin1String(key)).toString();
}

// A recording of values alone whose signal's name starts as a formula, which
// the CSV export refuses
inline QString formulaRecording(const QTemporaryDir& dir)
{
    const QString folder = dir.filePath(QStringLiteral("formula"));
    EXPECT_TRUE(QDir().mkpath(folder));
    writeFile(folder + QStringLiteral("/signals.csv"),
              "host_us,signal,value,unit\n100,=SUM(A1),1,rpm\n");
    return folder;
}

// A copy of a fixture under a name holding a vehicle's label
inline QString labeledCopy(const QTemporaryDir& dir, const char* source)
{
    const QString folder =
        dir.filePath(QStringLiteral("20261105-220322 My Weekend Car"));
    EXPECT_TRUE(QDir().mkpath(folder));
    const QDir from(fixture(source));
    for (const QString& name : from.entryList(QDir::Files)) {
        EXPECT_TRUE(QFile::copy(from.filePath(name), folder + '/' + name));
    }
    return folder;
}

} // namespace
