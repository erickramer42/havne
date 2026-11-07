// File/layer: app/recordingimport.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.Import* (test_bridge); QML: QmlTest.recordings
//
// Another logger's file made a recording (ADR-0054 sections 4 and 5): a
// candump log, or a CSV through its column map, imported by core's
// ecuio_import into the recordings folder, on a thread of its own. The
// Recordings page's Import picks the file; this class reads nothing of it
// but the first line, to guess its kind.

#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <functional>
#include <thread>

class RecordingImport : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    // ecuio_import's formats
    enum Kind { Candump = 1, Csv = 2 };
    Q_ENUM(Kind)

    // root gives the folder recordings are kept in, read at each import
    explicit RecordingImport(std::function<QString()> root,
                             QObject* parent = nullptr);
    // Waits for an import under way: core's has no cancel
    ~RecordingImport() override;
    RecordingImport(const RecordingImport&) = delete;
    RecordingImport& operator=(const RecordingImport&) = delete;

    bool busy() const { return m_busy; }

    // The file's kind from its first line that isn't blank: candump's
    // "(seconds) interface ID#data" is Candump, anything else Csv
    Q_INVOKABLE int guessKind(const QUrl& file) const;

    // Starts an import of file as kind, a CSV through the column map at
    // map, a candump log's interface named (empty: the log's only one).
    // False while one is under way. finished() follows, on this thread,
    // with { ok, words, folder, frames, values, columnsIgnored,
    // durationUs }: words core's refusal (a line and why, never a path) or
    // the app's, "" with ok
    Q_INVOKABLE bool start(const QUrl& file, int kind, const QUrl& map,
                           const QString& interfaceName);

    // A local file copied as a content:// one is, so a test reaches the copy
    void setCopyLocalFilesForTesting(bool copy) { m_copyLocal = copy; }

    // The largest column map read, as havne-cli reads one
    static constexpr qint64 MapMaxBytes = 1024 * 1024;

signals:
    void busyChanged();
    void finished(const QVariantMap& result);

private:
    void done(const QVariantMap& result);

    std::function<QString()> m_root;
    std::thread m_worker;
    bool m_busy = false;
    bool m_copyLocal = false;
};
