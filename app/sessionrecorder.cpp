// File/layer: app/sessionrecorder.cpp | Qt6 Core, app layer, C++17
// Testing: test_bridge (BridgeTest.Recording*, the vehicle's label in
//          the folder's name among them), through the bridge;
//          latency.csv: BridgeTest.ARecordingKeepsReplyTimingsSnapshots

#include "sessionrecorder.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QTime>

#include <algorithm>
#include <iterator>

namespace {

/* frames.csv's header row is its format version (ADR-0017): a reader
 * accepts only header rows it knows. */
constexpr char kFramesHeader[] = "host_us,timestamp_us,id,flags,data\n";
constexpr char kSignalsHeader[] = "host_us,signal,value,unit\n";

/* A CSV field, quoted only when it has to be. Names and units come from
 * signal sets, which may hold anything printable. */
QByteArray csvQuoted(QByteArray f)
{
    if (f.contains(',') || f.contains('"') || f.contains('\n')) {
        f.replace("\"", "\"\"");
        f = '"' + f + '"';
    }
    return f;
}

QByteArray csvField(const char* text, size_t cap)
{
    return csvQuoted(QByteArray(text, static_cast<qsizetype>(qstrnlen(text, cap))));
}

QByteArray hex(quint32 v)
{
    return "0x" + QByteArray::number(v, 16).toUpper();
}

/* The interval's clock as havne-cli's timing lines name it */
QByteArray clockName(uint32_t clock)
{
    switch (clock) {
    case ECUIO_TIMING_CLOCK_HOST:
        return "host";
    case ECUIO_TIMING_CLOCK_ADAPTER:
        return "adapter";
    case ECUIO_TIMING_CLOCK_KERNEL:
        return "kernel";
    default:
        return "none";
    }
}

/* A quantile, or empty when it has too few samples */
QByteArray quantile(uint32_t us, bool has)
{
    return has ? QByteArray::number(us) : QByteArray();
}

}  // namespace

bool SessionRecorder::start(const QString& root, const QStringList& header,
                            QString* error, const QString& label)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString tail = label.isEmpty() ? QString() : QLatin1Char(' ') + label;
    QDir dir(root);
    QString name = stamp + tail;
    for (int n = 2; dir.exists(name); ++n) {
        name = stamp + QLatin1Char('-') + QString::number(n) + tail;
    }
    if (!dir.mkpath(name)) {
        *error = QStringLiteral("can't create %1").arg(dir.filePath(name));
        return false;
    }
    m_folder = dir.filePath(name);

    m_frames.setFileName(m_folder + QStringLiteral("/frames.csv"));
    m_signals.setFileName(m_folder + QStringLiteral("/signals.csv"));
    m_events.setFileName(m_folder + QStringLiteral("/events.log"));
    for (QFile* f : { &m_frames, &m_signals, &m_events }) {
        if (!f->open(QIODevice::WriteOnly | QIODevice::Text)) {
            *error = QStringLiteral("can't create %1 (%2)").arg(f->fileName(), f->errorString());
            m_frames.close();
            m_signals.close();
            m_events.close();
            return false;
        }
    }

    m_t0 = Clock::now();
    m_latency.setFileName(m_folder + QStringLiteral("/latency.csv"));
    std::fill(std::begin(m_timingTotals), std::end(m_timingTotals), 0);
    m_frameRows = 0;
    m_sampleRows = 0;
    m_ok = true;
    m_active = true;

    write(m_frames, kFramesHeader);
    /* A byte-order mark, so a spreadsheet reads the units' UTF-8 ("°C")
     * as UTF-8. frames.csv is ASCII and needs none. */
    write(m_signals, QByteArray("\xEF\xBB\xBF") + kSignalsHeader);
    for (const QString& line : header) {
        write(m_events, line.toUtf8() + '\n');
    }
    write(m_events, "columns  local time, seconds since the recording started "
                    "(host_us / 1e6 in the CSV files), message\n\n");
    return true;
}

qint64 SessionRecorder::hostUs(Clock::time_point t) const
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t - m_t0).count();
    return us < 0 ? 0 : static_cast<qint64>(us);
}

void SessionRecorder::write(QFile& file, const QByteArray& line)
{
    if (m_ok && file.write(line) != line.size()) {
        m_ok = false;
    }
}

void SessionRecorder::frame(Clock::time_point arrived, const ecuio_frame_t& frame, uint16_t len)
{
    if (!m_active) return;
    QByteArray row = QByteArray::number(hostUs(arrived)) + ','
                   + QByteArray::number(static_cast<qulonglong>(frame.timestamp_us)) + ','
                   + hex(frame.arbitration_id) + ','
                   + hex(frame.flags) + ','
                   + QByteArray(reinterpret_cast<const char*>(frame.data), len).toHex(' ').toUpper()
                   + '\n';
    write(m_frames, row);
    ++m_frameRows;
}

void SessionRecorder::sample(Clock::time_point arrived, const ecuio_signal_def_t& def, double value)
{
    if (!m_active) return;
    QByteArray row = QByteArray::number(hostUs(arrived)) + ','
                   + csvField(def.name, sizeof def.name) + ','
                   + QByteArray::number(value, 'g', 12) + ','
                   + csvField(def.unit, sizeof def.unit)
                   + '\n';
    write(m_signals, row);
    ++m_sampleRows;
}

void SessionRecorder::sample(Clock::time_point arrived, const ecuio_signal_def_t& def,
                             const QString& text)
{
    if (!m_active) return;
    QByteArray row = QByteArray::number(hostUs(arrived)) + ','
                   + csvField(def.name, sizeof def.name) + ','
                   + csvQuoted(text.toUtf8()) + ','
                   + csvField(def.unit, sizeof def.unit)
                   + '\n';
    write(m_signals, row);
    ++m_sampleRows;
}

void SessionRecorder::event(const QString& line)
{
    if (!m_active) return;
    const qint64 us = hostUs(Clock::now());
    write(m_events, QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")).toUtf8()
                    + "  +" + QByteArray::number(static_cast<double>(us) / 1e6, 'f', 3)
                    + "  " + line.toUtf8() + '\n');
}

void SessionRecorder::timing(const ecuio_session_timing_t& t,
                             const ecuio_request_timing_t* rows, uint32_t count,
                             const uint32_t* rxIds, uint32_t channels)
{
    if (!m_active || count == 0)
        return;
    const QByteArray at =
        QByteArray::number(static_cast<double>(t.snapshot_us) / 1e6, 'f', 3);
    QByteArray block;
    for (uint32_t i = 0; i < count && i < ECUIO_PROFILE_MAX_REQUESTS; ++i) {
        const ecuio_request_timing_t& r = rows[i];
        const quint64 total = quint64{r.answered} + r.refused + r.timeouts;
        if (total == m_timingTotals[i])
            continue;
        m_timingTotals[i] = total;
        const bool latency = r.answered + r.refused > 0;
        const bool interval = r.intervals > 0;
        const uint8_t length = r.length <= 3 ? r.length : 3;
        block += at + ',' + hex(r.channel < channels ? rxIds[r.channel] : 0) +
                 ',' +
                 QByteArray(reinterpret_cast<const char*>(r.request), length)
                     .toHex(' ')
                     .toUpper() +
                 ',' + QByteArray::number(r.answered) + ',' +
                 QByteArray::number(r.refused) + ',' +
                 QByteArray::number(r.timeouts) + ',' +
                 QByteArray::number(r.packed) + ',' +
                 quantile(r.latency_p50_us, latency) + ',' +
                 quantile(r.latency_p99_us,
                          (r.flags & ECUIO_TIMING_LATENCY_P99) != 0) +
                 ',' +
                 quantile(r.latency_p999_us,
                          (r.flags & ECUIO_TIMING_LATENCY_P999) != 0) +
                 ',' + quantile(r.latency_max_us, latency) + ',' +
                 QByteArray::number(r.intervals) + ',' +
                 QByteArray::number(r.intervals_mixed) + ',' +
                 quantile(r.interval_p50_us, interval) + ',' +
                 quantile(r.interval_p99_us,
                          (r.flags & ECUIO_TIMING_INTERVAL_P99) != 0) +
                 ',' +
                 quantile(r.interval_p999_us,
                          (r.flags & ECUIO_TIMING_INTERVAL_P999) != 0) +
                 ',' + quantile(r.interval_max_us, interval) + ',' +
                 clockName(t.interval_clock) + '\n';
    }
    if (block.isEmpty())
        return;
    if (!m_latency.isOpen()) {
        if (!m_latency.open(QIODevice::WriteOnly | QIODevice::Text)) {
            m_ok = false;
            return;
        }
        write(m_latency, QByteArray(ECUIO_TIMING_COLUMNS) + '\n');
    }
    write(m_latency, block);
}

void SessionRecorder::flush()
{
    if (!m_active) return;
    if (m_ok && m_latency.isOpen() && !m_latency.flush()) {
        m_ok = false;
    }
    for (QFile* f : { &m_frames, &m_signals, &m_events }) {
        if (m_ok && !f->flush()) {
            m_ok = false;
        }
    }
}

void SessionRecorder::stop(const QString& summary)
{
    if (!m_active) return;
    write(m_events, '\n' + summary.toUtf8() + '\n');
    flush();
    m_frames.close();
    m_signals.close();
    m_events.close();
    m_latency.close();
    m_active = false;
}
