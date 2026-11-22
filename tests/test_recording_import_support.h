// File/layer: tests/test_recording_import_support.h | GoogleTest, Qt6 Core, bridge tier
// Testing: BRIDGE: the helpers of test_recording_import*.cpp (test_bridge)

#pragma once

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <utility>

#include "recordingimport.h"
#include "sessionbridge.h"

namespace {

// Runs the GUI thread's events until done() or ms pass; done()'s answer
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
    return QStringLiteral(ECUIO_IMPORT_FIXTURES "/csv/") + QLatin1String(name);
}

inline QUrl fileUrl(const QString& path)
{
    return QUrl::fromLocalFile(path);
}

// A file of the test's own, its text as given
inline QString write(const QTemporaryDir& dir, const char* name,
              const QByteArray& text)
{
    const QString path = dir.filePath(QLatin1String(name));
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(text);
    return path;
}

// An import run to its end: its result, and how often busy changed
struct Imported {
    QVariantMap result;
    int busyChanges = 0;
    bool started = false;
};

inline Imported import(RecordingImport& im, const QUrl& file, int kind,
                const QUrl& map = {}, const QString& iface = {})
{
    Imported out;
    bool done = false;
    QObject context;
    QObject::connect(&im, &RecordingImport::busyChanged, &context,
                     [&out] { ++out.busyChanges; });
    QObject::connect(&im, &RecordingImport::finished, &context,
                     [&](const QVariantMap& r) {
                         out.result = r;
                         done = true;
                     });
    out.started = im.start(file, kind, map, iface);
    if (out.started) {
        EXPECT_TRUE(im.busy());
        EXPECT_TRUE(waitUntil([&] { return done; })) << "the import ends";
        EXPECT_FALSE(im.busy());
    }
    return out;
}

inline QString words(const Imported& i)
{
    return i.result.value(QStringLiteral("words")).toString();
}

inline bool ok(const Imported& i)
{
    return i.result.value(QStringLiteral("ok")).toBool();
}

inline QStringList entries(const QString& folder)
{
    return QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                  QDir::Name);
}

// A guess run to its end
inline QVariantMap guessed(RecordingImport& im, const QUrl& file,
                    const QString& unit = {})
{
    QVariantMap out;
    bool done = false;
    QObject context;
    QObject::connect(&im, &RecordingImport::guessFinished, &context,
                     [&](const QVariantMap& r) {
                         out = r;
                         done = true;
                     });
    EXPECT_TRUE(im.guess(file, unit));
    EXPECT_TRUE(im.busy());
    EXPECT_TRUE(waitUntil([&] { return done; })) << "the guess ends";
    EXPECT_FALSE(im.busy());
    return out;
}

inline QVariantMap column(const QVariantMap& g, int i)
{
    return g.value(QStringLiteral("columns")).toList().at(i).toMap();
}

inline QString text(const QString& path)
{
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly));
    return QString::fromUtf8(f.readAll()).remove(QLatin1Char('\r'));
}

} // namespace
