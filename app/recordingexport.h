// File/layer: app/recordingexport.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: RecordingExportTest (test_bridge); QML: QmlTest.recordings,
//          QmlTest.phone
//
// The Recordings viewer's Export (ADR-0056 sections 5 and 7): the whole of a
// recording written by core's ecuio_export as pcapng, a wide or long CSV or a
// folder copy, and the wide CSV's size per row interval
// (ecuio_export_estimate), each on a thread of its own. Core writes paths and
// refuses one that exists, while a picker hands back a content:// URI on
// Android, so a file is written into a temporary folder and copied to the
// place picked. The object's end stops both through core's cancel
// (ecuio_export_with) and the copy's own check, so quitting mid-export, or
// Android closing the app, waits only for the next ask.

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <optional>
#include <thread>

class RecordingExport : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool sizing READ sizing NOTIFY sizingChanged)

public:
    // ecuio_export_options_t.format
    enum Format { Folder = 1, Csv = 2, Pcapng = 3 };
    Q_ENUM(Format)

    explicit RecordingExport(QObject* parent = nullptr);
    // Stops the workers, then waits for them: what they had begun is
    // removed and nothing is said
    ~RecordingExport() override;
    RecordingExport(const RecordingExport&) = delete;
    RecordingExport& operator=(const RecordingExport&) = delete;

    bool busy() const { return m_busy; }
    bool sizing() const { return m_sizing; }

    // The name an export of this recording's folder is offered: its stamp
    // alone, or "recording", never the rest of the folder's name, which can
    // hold the vehicle's label
    Q_INVOKABLE static QString fileStem(const QString& folder);
    // Whether the folder holds a signals.csv, what a CSV export reads
    Q_INVOKABLE static bool holdsValues(const QString& folder);
    // At startup: removes the folders an export killed mid-way left in
    // tempPath (raw, they can hold a VIN), each named havne-export- and six
    // letters or digits and last changed more than a day before now, and
    // nothing else; the count removed
    static int removeStaleCopies(const QString& tempPath, const QDateTime& now);

    // Sizes the wide CSV for each of rowMs (0 a row a sweep, at most 64), the
    // columns named (none: core's rule). One runs at a time: a newer call
    // replaces one still waiting, and a call made while one runs drops that
    // one's answer. False for an empty folder or list. estimated() follows,
    // on this thread, with { ok, words, columns, sizes: [{ rowMs, rows, bytes
    // }] }
    Q_INVOKABLE bool estimate(const QString& folder, const QVariantList& rowMs,
                              int units, const QStringList& columns);
    // Forgets an estimate waiting or running: no estimated() for it
    Q_INVOKABLE void dropEstimate();

    // Starts the export options describes: { folder, format (Format),
    // longLayout, rowMs, units, columns, raw }. target is the file picked, or
    // for Folder the folder to make the copy in. False while one runs or for
    // a bad format. finished() follows, on this thread, with { ok, words,
    // folder (the recording's, to match it to the view), name (a folder
    // copy's), summary, line, frames, values, signals, rows, files }: words
    // core's refusal or the app's, summary and line what was written, none
    // holding a path
    Q_INVOKABLE bool start(const QVariantMap& options, const QUrl& target);

    // A local target written as a content:// one is, so a test reaches that
    // path
    void setThroughProviderForTesting(bool on) { m_throughProvider = on; }
    // The workers run out of memory at once
    void setFailWorkerForTesting(bool on) { m_failWorkerForTesting = on; }
    // Where a worker waits, up to ms or until the stop, as a long export
    // would: at core's first ask, or between core's export and the copy
    enum class HoldForTesting { None, FirstAsk, BeforeCopy };
    void setHoldForTesting(HoldForTesting where, int ms)
    {
        m_holdForTesting = where;
        m_holdMsForTesting = ms;
    }
    // The workers that have come to their hold
    int heldForTesting() const { return m_heldForTesting.load(); }
    // Every copy to the target fails, as a provider refusing the bytes
    void setFailCopyForTesting(bool on) { m_failCopyForTesting = on; }

signals:
    void busyChanged();
    void sizingChanged();
    void estimated(const QVariantMap& result);
    void finished(const QVariantMap& result);

private:
    struct EstimateJob {
        QByteArray folder;
        QList<quint32> rowMs;
        quint32 units = 0;
        QList<QByteArray> columns;
    };

    void launchEstimate(const EstimateJob& job, quint64 generation);
    void estimateDone(const QVariantMap& answer, quint64 generation);
    void exportDone(const QVariantMap& answer, const QByteArray& folder);

    std::thread m_worker;
    std::thread m_estimateWorker;
    // Set once, by the destructor; read by the workers
    std::atomic<bool> m_stopping{false};
    std::optional<EstimateJob> m_waitingEstimate;
    quint64 m_estimateGeneration = 0;
    quint64 m_waitingGeneration = 0;
    bool m_busy = false;
    bool m_sizing = false;
    bool m_throughProvider = false;
    bool m_failWorkerForTesting = false;
    HoldForTesting m_holdForTesting = HoldForTesting::None;
    int m_holdMsForTesting = 0;
    std::atomic<int> m_heldForTesting{0};
    bool m_failCopyForTesting = false;
};
