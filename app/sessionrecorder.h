// File/layer: app/sessionrecorder.h | Qt6 Core, app layer, C++17
// Testing: test_bridge (BridgeTest.Recording*, the vehicle's label in
//          the folder's name among them), through the bridge

#pragma once

#include <QFile>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstdint>

#include "ecuio/core.h"
#include "ecuio/signals.h"

/* One recording per connect (ADR-0017): a folder holding frames.csv
 * (every frame the bridge receives), signals.csv (every decoded value)
 * and events.log (a header, then the activity lines). Times are host
 * microseconds since the recording started, from the steady clock, so a
 * vendor's timestamps can be checked against them. GUI-thread-only, like
 * the rest of the bridge. */
class SessionRecorder
{
public:
    using Clock = std::chrono::steady_clock;

    /* Creates <root>/<yyyyMMdd-HHmmss>[-n][ <label>]/ and its three
     * files, then writes header (one line each) into events.log. The
     * label (ADR-0027 §2) comes as a folder name may take it
     * (SessionBridge::folderLabel); "" leaves the date alone. False, with
     * *error set, if the folder or a file can't be created. */
    bool start(const QString& root, const QStringList& header, QString* error,
               const QString& label = {});
    bool active() const { return m_active; }
    /* False once a write has failed; the bridge then stops the recording. */
    bool ok() const { return m_ok; }

    void frame(Clock::time_point arrived, const ecuio_frame_t& frame, uint16_t len);
    void sample(Clock::time_point arrived, const ecuio_signal_def_t& def, double value);
    // A text signal's value, in the same column, CSV-quoted when it needs it
    void sample(Clock::time_point arrived, const ecuio_signal_def_t& def, const QString& text);
    void event(const QString& line);
    void flush();
    /* Writes the summary to events.log and closes the files. */
    void stop(const QString& summary);

    /* The current or last recording's folder, absolute; empty before the first. */
    QString folder() const { return m_folder; }
    qint64 frameRows() const { return m_frameRows; }
    qint64 sampleRows() const { return m_sampleRows; }

private:
    qint64 hostUs(Clock::time_point t) const;
    void write(QFile& file, const QByteArray& line);

    QFile m_frames;
    QFile m_signals;
    QFile m_events;
    Clock::time_point m_t0{};
    QString m_folder;
    qint64 m_frameRows = 0;
    qint64 m_sampleRows = 0;
    bool m_active = false;
    bool m_ok = true;
};
