// File/layer: app/recordingframes.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: ViewerFramesTest (test_bridge)

#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "ecuio/recording.h"
#include "sessionbridge.h"

// The recording viewer's Frames table (ADR-0049's appendix "frames read
// back"): the frames.csv rows whose host_us lies in the part of the
// recording in view, read through ecuio_recording_read_frames on a thread
// of its own and handed to a FrameListModel and a FrameIdModel the page
// shows through the Log page's table components. Times are milliseconds
// since the recording started (host_us / 1000), as RecordingView's are;
// each row's time in the models is its host_us, never the frame's own
// session time, so the table's times and its filter's span read on the
// viewer's axis.
//
// A read gives at most MaxRows rows, the first from the part's left edge;
// note then says so. load() while a read runs stops that one (its rows
// never reach the models) and starts the next.
class RecordingFrames : public QObject
{
    Q_OBJECT
    Q_PROPERTY(FrameListModel* frames READ frames CONSTANT)
    Q_PROPERTY(FrameIdModel* ids READ ids CONSTANT)
    // A read is running
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    // The rows the models hold
    Q_PROPERTY(int rows READ rows NOTIFY loadedChanged)
    // "The first 10,000 frames from 12.3 s: zoom in for the rest" when the
    // part had more rows than MaxRows; "" otherwise
    Q_PROPERTY(QString note READ note NOTIFY loadedChanged)
    // Core's words for a read that failed (a row it refuses, no
    // frames.csv); "" otherwise. The models are empty then
    Q_PROPERTY(QString error READ error NOTIFY loadedChanged)

public:
    // The models' ring size: the rows of one read
    static constexpr int MaxRows = FrameListModel::MaxFrames;

    explicit RecordingFrames(QObject* parent = nullptr);
    ~RecordingFrames() override;

    FrameListModel* frames() { return &m_frames; }
    FrameIdModel* ids() { return &m_ids; }
    bool loading() const { return m_loading; }
    int rows() const { return m_rows; }
    QString note() const { return m_note; }
    QString error() const { return m_error; }

    // Reads the rows of the recording in folder from fromMs to toMs, both
    // included; a view's edges, so a fromMs before the start is the start
    // and a toMs at the start or before it is a view with no rows (the
    // models empty, no read). Stops a read under way first.
    // The models fill on the GUI thread when the read ends; loadedChanged
    // follows, and a stopped read ends silently
    Q_INVOKABLE void load(const QString& folder, double fromMs, double toMs);
    // Stops a read under way and empties the models
    Q_INVOKABLE void clear();

    // The read, replaceable so a test can hold one in flight; the
    // parameters are ecuio_recording_read_frames's
    using ReadFn = std::function<ecuio_status_t(const char*, uint64_t, uint64_t,
                                                ecuio_frame_row_callback_t,
                                                void*, ecuio_frames_read_t*)>;
    void setReadForTesting(ReadFn read) { m_read = std::move(read); }
    // How many reads have ended and been taken on the GUI thread, those
    // dropped for a newer one among them
    int finishedForTesting() const { return m_finishedCount; }

signals:
    void loadingChanged();
    void loadedChanged();

private:
    // One row as the worker keeps it: never an ecuio_frame_t, whose
    // 4,120 bytes at MaxRows rows would be 41 MB
    struct Row {
        quint32 id = 0;
        quint32 flags = 0;
        quint64 hostUs = 0;
        QByteArray data;
    };
    struct Job {
        quint64 generation = 0;
        QByteArray folder;
        quint64 fromUs = 0;
        quint64 toUs = 0;
        double fromMs = 0;
        std::atomic<bool> stop{false};
        // The worker's alone until it posts its end
        std::vector<Row> rows;
        bool capped = false;
        ecuio_status_t rc = ECUIO_OK;
        QString detail;
        std::thread worker;
    };

    static void run(const std::shared_ptr<Job>& job, const ReadFn& read,
                    RecordingFrames* self);
    void finished(const std::shared_ptr<Job>& job);
    void stopAll();
    void setLoading(bool loading);

    FrameListModel m_frames;
    FrameIdModel m_ids;
    ReadFn m_read;
    // Every job whose thread hasn't been joined, the newest last
    std::vector<std::shared_ptr<Job>> m_jobs;
    quint64 m_generation = 0;
    bool m_loading = false;
    int m_rows = 0;
    int m_finishedCount = 0;
    QString m_note;
    QString m_error;
};
