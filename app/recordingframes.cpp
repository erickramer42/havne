// File/layer: app/recordingframes.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: ViewerFramesTest (test_bridge)

#include "recordingframes.h"

#include <QLocale>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Milliseconds since the recording started to its host_us; a negative
// time is the start
quint64 microseconds(double ms)
{
    return ms <= 0 ? 0 : static_cast<quint64>(std::llround(ms * 1000.0));
}

} // namespace

RecordingFrames::RecordingFrames(QObject* parent)
    : QObject(parent), m_ids(&m_frames)
{
    // Oldest first: the part in view reads down the table in time
    m_frames.setNewestFirst(false);
    m_read = [](const char* folder, uint64_t from, uint64_t to,
                ecuio_frame_row_callback_t cb, void* user,
                ecuio_frames_read_t* summary) {
        return ecuio_recording_read_frames(folder, from, to, cb, user, summary);
    };
}

RecordingFrames::~RecordingFrames()
{
    stopAll();
    for (const std::shared_ptr<Job>& j : m_jobs) {
        if (j->worker.joinable()) {
            j->worker.join();
        }
    }
}

void RecordingFrames::stopAll()
{
    for (const std::shared_ptr<Job>& j : m_jobs) {
        j->stop = true;
    }
}

void RecordingFrames::setLoading(bool loading)
{
    if (m_loading != loading) {
        m_loading = loading;
        emit loadingChanged();
    }
}

void RecordingFrames::load(const QString& folder, double fromMs, double toMs)
{
    const quint64 toUs = microseconds(toMs);
    if (toUs == 0) {
        // A view that ends at the recording's start or before it has no
        // rows (core reads a to of 0 as the end, which no view means)
        clear();
        return;
    }
    stopAll();
    const double from = std::max(fromMs, 0.0);
    auto job = std::make_shared<Job>();
    job->generation = ++m_generation;
    job->folder = folder.toUtf8();
    job->fromUs = microseconds(from);
    job->toUs = std::max(toUs, job->fromUs);
    job->fromMs = from;
    m_jobs.push_back(job);
    setLoading(true);
    const ReadFn read = m_read;
    try {
        job->worker = std::thread([job, read, this] { run(job, read, this); });
    } catch (...) {
        // No thread to read on: nothing is running, and the page is told
        m_jobs.pop_back();
        setLoading(false);
        m_frames.clear();
        m_ids.refresh();
        m_rows = 0;
        m_note.clear();
        m_error = tr("the frames could not be read: no thread to read on");
        emit loadedChanged();
    }
}

void RecordingFrames::clear()
{
    stopAll();
    ++m_generation; // whatever is running posts to nobody
    setLoading(false);
    m_frames.clear();
    m_ids.refresh();
    m_rows = 0;
    m_note.clear();
    m_error.clear();
    emit loadedChanged();
}

void RecordingFrames::run(const std::shared_ptr<Job>& job, const ReadFn& read,
                          RecordingFrames* self)
{
    struct State {
        Job* job;
        bool outOfMemory = false;
    } state{job.get()};
    // Called from C: nothing may throw out of it
    auto take = [](void* user, const ecuio_frame_t* f, uint64_t hostUs) -> int {
        State* s = static_cast<State*>(user);
        Job& j = *s->job;
        try {
            if (j.stop.load()) {
                return 1;
            }
            if (j.rows.size() >= static_cast<size_t>(MaxRows)) {
                // One row past the cap: there is more to show than fits
                j.capped = true;
                return 1;
            }
            Row r;
            r.id = f->arbitration_id;
            r.flags = f->flags;
            r.hostUs = hostUs;
            r.data = QByteArray(reinterpret_cast<const char*>(f->data),
                                f->data_length);
            j.rows.push_back(std::move(r));
            return 0;
        } catch (...) {
            s->outOfMemory = true;
            return 1;
        }
    };
    ecuio_frames_read_t summary{};
    job->rc = read(job->folder.constData(), job->fromUs, job->toUs, take,
                   &state, &summary);
    if (state.outOfMemory) {
        job->rc = ECUIO_ERR_OUT_OF_MEMORY;
        job->detail = tr("out of memory");
    } else if (job->rc != ECUIO_OK &&
               !(job->rc == ECUIO_ERR_CANCELED && job->capped)) {
        const char* detail = ecuio_last_error_detail();
        job->detail = detail != nullptr ? QString::fromUtf8(detail) : QString();
    }
    QMetaObject::invokeMethod(
        self, [self, job] { self->finished(job); }, Qt::QueuedConnection);
}

void RecordingFrames::finished(const std::shared_ptr<Job>& job)
{
    ++m_finishedCount;
    if (job->worker.joinable()) {
        job->worker.join(); // it posted this as its last act
    }
    m_jobs.erase(std::remove(m_jobs.begin(), m_jobs.end(), job), m_jobs.end());
    if (job->generation != m_generation) {
        return; // a newer read, or clear(), has the models
    }
    setLoading(false);
    m_frames.clear();
    m_error.clear();
    m_note.clear();
    const bool failed =
        job->rc != ECUIO_OK && !(job->rc == ECUIO_ERR_CANCELED && job->capped);
    if (failed) {
        m_error = job->detail.isEmpty() ? tr("the frames could not be read")
                                        : job->detail;
    } else {
        ecuio_frame_t f;
        for (const Row& r : job->rows) {
            f = ecuio_frame_t{};
            f.timestamp_us = r.hostUs;
            f.arbitration_id = r.id;
            f.flags = r.flags;
            f.data_length = static_cast<uint16_t>(r.data.size());
            std::memcpy(f.data, r.data.constData(),
                        static_cast<size_t>(r.data.size()));
            m_frames.appendFrame(f, f.data_length);
        }
        if (job->capped) {
            m_note = tr("the first %1 frames from %2 s: zoom in for the rest")
                         .arg(QLocale(QLocale::English).toString(MaxRows))
                         .arg(job->fromMs / 1000.0, 0, 'f', 1);
        }
    }
    m_rows = m_frames.heldCount();
    m_ids.refresh();
    emit loadedChanged();
}
