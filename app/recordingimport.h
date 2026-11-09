// File/layer: app/recordingimport.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.Import* (test_bridge); QML: QmlTest.recordings
//
// Another logger's file made a recording (ADR-0054 sections 4 and 5): a
// candump log, or a CSV through its column map, imported by core's
// ecuio_import into the recordings folder, on a thread of its own. The
// Recordings page's Import picks the file; this class reads of it only the
// first line (to guess its kind) and, for a CSV, core's guess of its column
// map with the first rows, for the panel to show.

#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVariantList>
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

    // Starts a CSV import through a column map given as text (the Import
    // panel's edited guess): the same worker, finished() and result as
    // start(), with no map file read
    Q_INVOKABLE bool startWithMap(const QUrl& file, const QString& mapText);

    // Guesses a CSV's column map from its header (core's
    // ecuio_import_csv_guess, on the worker), timeUnit "", "s", "ms" or
    // "us". False while an import or a guess is under way.
    // guessFinished() follows, on this thread, with { ok, words, mapText
    // (the map core wrote), unitKnown, timeNumbers, decimalComma,
    // preamble, delimiter, headerRow, timeColumn, timeUnit ("" for
    // none), columns: [{ index, column, used, why, isTime, name, unit,
    // kind, canPutBack, samples }], notes }. samples are the file's first
    // rows below the header for display, read here and never judged:
    // core's reader is the import's, this one only shows rows.
    Q_INVOKABLE bool guess(const QUrl& file, const QString& timeUnit);

    // The map text with the panel's edits: edits is one { column, include,
    // name, unit, kind } per header column in the header's order, timeUnit
    // "" to keep the map's. A column in the base map keeps its other keys
    // (decimals, rank, group); one put back is { column, name, kind }. An
    // edit of the map's time column is skipped: it is the time, never a
    // signal.
    // Returns "" when mapText isn't a JSON object. The map is not checked
    // here: ecuio_import refuses a bad name or unit with its words.
    Q_INVOKABLE QString composeMap(const QString& mapText,
                                   const QVariantList& edits,
                                   const QString& timeUnit) const;

    // A column map saved under <settings folder>/maps/<name>.json, name
    // made safe (letters, digits, - and _). { ok, words, name } ; a name
    // taken is refused, never overwritten
    Q_INVOKABLE QVariantMap saveMap(const QString& name,
                                    const QString& mapText);
    // The saved maps, by name: [{ name, url }]
    Q_INVOKABLE QVariantList savedMaps() const;
    // The folder maps are kept in; a test points it at a temporary one
    void setMapsRootForTesting(const QString& root) { m_mapsRoot = root; }

    // A local file copied as a content:// one is, so a test reaches the copy
    void setCopyLocalFilesForTesting(bool copy) { m_copyLocal = copy; }

    // The largest column map read, as havne-cli reads one
    static constexpr qint64 MapMaxBytes = 1024 * 1024;

signals:
    void busyChanged();
    void finished(const QVariantMap& result);
    void guessFinished(const QVariantMap& result);

private:
    // One job at a time on the worker; its result goes to finished() or,
    // for a guess, guessFinished()
    bool beginImport(const QUrl& file, int kind, const QUrl& map,
                     const QByteArray& mapText, bool hasMapText,
                     const QString& interfaceName);
    bool launch(bool isGuess, std::function<QVariantMap()> work);
    void done(const QVariantMap& result, bool isGuess);
    QString mapsRoot() const;

    std::function<QString()> m_root;
    std::thread m_worker;
    bool m_busy = false;
    bool m_copyLocal = false;
    QString m_mapsRoot;
};
