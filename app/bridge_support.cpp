// File/layer: app/bridge_support.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "bridgetext.h"
#include "refused_signals.h" /* cli/: the signals an ECU refuses, as havne-cli says them */
#include "default_pick.h" /* cli/: the default pick, as havne-cli's --pick ranked */
#include "scan_line.h"       /* cli/: when a module's session earns a line */
#include "ecuio/signal_sets.h"

#include <QStringList>
#include <QtGlobal>

#include <algorithm>

using namespace bridgetext;

void SessionBridge::writePickLine()
{
    // A pick core took is in the recording as core's own pick line; a
    // whole set's line says why, which core's line doesn't
    if (m_pickLineCore) {
        logUnnoted(m_pickLine);
    } else {
        emit logMessage(m_pickLine);
    }
}

SessionBridge::DeliveryStats SessionBridge::deliveryStats() const
{
    DeliveryStats s;
    s.frames = m_framesDelivered.load(std::memory_order_relaxed);
    s.dropped = static_cast<quint64>(m_droppedFrames.load(std::memory_order_relaxed));
    const int queued = m_inFlightFrames.load(std::memory_order_relaxed);
    s.queuedAtEnd = queued > 0 ? static_cast<quint64>(queued) : 0;
    s.values = m_valuesStaged;
    s.valuesMissed = m_valuesDropped;
    s.frameToGui = m_frameLatency;
    s.valueToModel = m_valueLatency;
    return s;
}

namespace {

// A latency for the activity log: us below a millisecond, then ms with
// one decimal below 10 ms, then whole ms
QString latencyText(uint64_t us)
{
    if (us < 1000) return QStringLiteral("%1 µs").arg(us);
    if (us < 10000) {
        return QStringLiteral("%1 ms").arg(static_cast<double>(us) / 1000.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 ms").arg((us + 500) / 1000);
}

QString quantilesText(const LatencyStats& l)
{
    return QStringLiteral("median %1, p99 %2, max %3")
        .arg(latencyText(l.quantileUs(0.5)), latencyText(l.quantileUs(0.99)),
             latencyText(l.maxUs()));
}

} // namespace

// "Delivery: 7074 frames, none dropped; to the GUI thread in median
// 31 us, p99 412 us, max 1.2 ms; 28296 values, to the model in median
// 51 ms, p99 99 ms, max 101 ms". A quantile is the top of its bucket,
// at most 12.5 % above the true value (LatencyStats).
QString SessionBridge::deliveryText(const DeliveryStats& s)
{
    if (s.frames == 0) {
        return QStringLiteral("Delivery: no frames");
    }
    QString line = QStringLiteral("Delivery: %1 frames, ").arg(s.frames);
    if (s.dropped == 0) {
        line += QStringLiteral("none dropped");
    } else {
        line += QStringLiteral("%1 dropped at the in-flight cap (%2 %)")
                    .arg(s.dropped)
                    .arg(100.0 * static_cast<double>(s.dropped) / static_cast<double>(s.frames),
                         0, 'f', 1);
    }
    if (s.queuedAtEnd > 0) {
        line += QStringLiteral(", %1 still queued at the end").arg(s.queuedAtEnd);
    }
    if (s.frameToGui.count() > 0) {
        line += QStringLiteral("; to the GUI thread in ") + quantilesText(s.frameToGui);
    }
    if (s.values > 0) {
        line += QStringLiteral("; %1 values").arg(s.values);
        if (s.valueToModel.count() > 0) {
            line += QStringLiteral(", to the model in ") + quantilesText(s.valueToModel);
        }
        if (s.valuesMissed > 0) {
            line += QStringLiteral(" (%1 past the staging's limit, not timed)")
                        .arg(s.valuesMissed);
        }
    }
    return line;
}

// "Reply timing: 23 requests, 4512 answered, none refused, 2 timed out;
// latency P50 4.2 ms to 5.3 ms; intervals on the adapter's clock; ISO-TP:
// 640 messages, 40 segmented, none dropped, none unusable". Each
// request's figures are in the recording's latency.csv.
QString
BridgeFormats::timingText(const ecuio_session_timing_t& t,
                          const std::vector<ecuio_request_timing_t>& rows)
{
    uint32_t asked = 0;
    quint64 answered = 0;
    quint64 refused = 0;
    quint64 timeouts = 0;
    uint32_t low = UINT32_MAX;
    uint32_t high = 0;
    for (const ecuio_request_timing_t& r : rows) {
        if (r.answered + r.refused + r.timeouts == 0) {
            continue;
        }
        ++asked;
        answered += r.answered;
        refused += r.refused;
        timeouts += r.timeouts;
        if (r.answered + r.refused > 0) {
            low = std::min(low, r.latency_p50_us);
            high = std::max(high, r.latency_p50_us);
        }
    }
    if (asked == 0) {
        return {};
    }
    const auto count = [](quint64 n) {
        return n == 0 ? QStringLiteral("none") : QString::number(n);
    };
    QString line =
        QStringLiteral(
            "Reply timing: %1 requests, %2 answered, %3 refused, %4 timed out")
            .arg(asked)
            .arg(answered)
            .arg(count(refused), count(timeouts));
    if (low != UINT32_MAX) {
        line += low == high
                    ? QStringLiteral("; latency P50 %1").arg(latencyText(low))
                    : QStringLiteral("; latency P50 %1 to %2")
                          .arg(latencyText(low), latencyText(high));
    }
    switch (t.interval_clock) {
    case ECUIO_TIMING_CLOCK_ADAPTER:
        line += QStringLiteral("; intervals on the adapter's clock");
        break;
    case ECUIO_TIMING_CLOCK_KERNEL:
        line += QStringLiteral("; intervals on the kernel's receive times");
        break;
    case ECUIO_TIMING_CLOCK_HOST:
        line += QStringLiteral("; intervals on the host's clock");
        break;
    default:
        break;
    }
    if (t.flags & ECUIO_TIMING_ISOTP) {
        line +=
            QStringLiteral(
                "; ISO-TP: %1 messages, %2 segmented, %3 dropped, %4 unusable")
                .arg(t.isotp_completed)
                .arg(t.isotp_first_frames)
                .arg(count(t.isotp_dropped), count(t.isotp_malformed));
    }
    return line;
}

/* What each channel's ECU said it supports (ADR-0015 section 4), in the
 * activity log once per update: the probe's walk ending, and each
 * request it drops. Generation 0 is nothing learned yet. */
void SessionBridge::pollChannelSupport()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    bool changed = false;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_support_t s{};
        if (ecuio_session_get_channel_support(m_session, c, &s) != ECUIO_OK) {
            continue;
        }
        if (s.generation == m_supportGeneration[c]) {
            continue;
        }
        m_supportGeneration[c] = s.generation;
        m_supportReports[c] = s;
        changed = true;
        // The session's change first (ADR-0060): it's why this report moved
        SessionWritten& w = m_sessionWritten[c];
        cli::SessionSeen seen{w.state, w.nrc, w.entries, w.lost, w.written};
        if (cli::session_due(s, seen)) {
            w = {seen.state, seen.nrc, seen.entries, seen.lost, seen.written};
            const QString line = channelSessionText(s);
            if (!line.isEmpty()) {
                emit logMessage(line);
            }
        }
        emit logMessage(channelSupportText(s));
    }
    // Live says which signals their ECU refused, and with what (ADR-0051
    // section 3): the drops a negative reply made, by the defs they read
    if (changed) {
        std::vector<int> noRoom;
        for (uint32_t c = 0; c < m_channelCount; ++c) {
            if ((m_supportReports[c].flags & ECUIO_SUPPORT_NO_ROOM) != 0) {
                noRoom.push_back(static_cast<int>(c));
            }
        }
        m_signalModel->applyNoRoom(noRoom);
        std::vector<SignalListModel::Refusal> refused;
        for (const cli::RefusedSignal& r :
             cli::refused_signals(ecuio_session_signal_set(m_session),
                                  m_supportReports, m_channelCount)) {
            refused.push_back({r.id, refusalText(r.request[0], r.nrc),
                               refusalCode(r.request[0], r.nrc)});
        }
        m_signalModel->applyRefused(refused);
        std::vector<std::pair<uint32_t, uint32_t>> more;
        for (const cli::RefusedMore& m :
             cli::refused_unlisted(ecuio_session_signal_set(m_session),
                                   m_supportReports, m_channelCount)) {
            more.emplace_back(m.rx_id, m.count);
        }
        const QString unnamed = refusedUnnamedText(more);
        if (unnamed != m_refusedUnnamed) {
            m_refusedUnnamed = unnamed;
            emit refusedUnnamedChanged();
        }
    }
    // Live hides the rows core judges unsupported from the reports
    // (ADR-0039 section 6)
    uint32_t count = 0;
    if (changed && ecuio_session_get_unsupported_signals(
                       m_session, m_unsupportedRead.data(),
                       static_cast<uint32_t>(m_unsupportedRead.size()),
                       &count) == ECUIO_OK) {
        count = std::min<uint32_t>(
            count, static_cast<uint32_t>(m_unsupportedRead.size()));
        m_signalModel->applyUnsupported(std::vector<uint16_t>(
            m_unsupportedRead.begin(), m_unsupportedRead.begin() + count));
    }
}

// Live hides the signals core reports its frames come without, as it
// hides an unsupported PID's (ADR-0039 section 6): the fact is core's, the
// hiding the model's
void SessionBridge::pollAbsentSignals()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        m_absentRead.empty()) {
        return;
    }
    uint32_t count = 0;
    if (ecuio_session_get_absent_signals(m_session, m_absentRead.data(),
                                         static_cast<uint32_t>(m_absentRead.size()),
                                         &count) != ECUIO_OK) {
        return;
    }
    count = std::min<uint32_t>(count, static_cast<uint32_t>(m_absentRead.size()));
    if (count == m_absentIds.size() &&
        std::equal(m_absentIds.begin(), m_absentIds.end(), m_absentRead.begin())) {
        return;
    }
    m_absentIds.assign(m_absentRead.begin(), m_absentRead.begin() + count);
    m_signalModel->applyAbsent(m_absentIds);
}

void SessionBridge::pollUnaskedSignals()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        m_unaskedRead.empty()) {
        return;
    }
    uint32_t count = 0;
    if (ecuio_session_get_unasked_signals(
            m_session, m_unaskedRead.data(),
            static_cast<uint32_t>(m_unaskedRead.size()), &count) != ECUIO_OK) {
        return;
    }
    count =
        std::min<uint32_t>(count, static_cast<uint32_t>(m_unaskedRead.size()));
    if (count == m_unaskedIds.size() &&
        std::equal(m_unaskedIds.begin(), m_unaskedIds.end(),
                   m_unaskedRead.begin())) {
        return;
    }
    m_unaskedIds.assign(m_unaskedRead.begin(), m_unaskedRead.begin() + count);
    m_signalModel->applyUnasked(m_unaskedIds);
}

void SessionBridge::setPickGauges(const QVariantList& cells)
{
    if (cells == m_pickGauges) {
        return;
    }
    m_pickGauges = cells;
    m_pickDirty = true;
    emit pickGaugesChanged();
}

std::vector<uint16_t> BridgeFormats::gaugeIds(const ecuio_signal_set_t* set,
                                              const ecuio_signal_file_t* file,
                                              const QVariantList& cells)
{
    std::vector<uint16_t> ids;
    if (set == nullptr || set->defs == nullptr) {
        return ids;
    }
    // Each def's refs as SignalHub keys them
    QHash<QString, uint16_t> byRef;
    const bool builtIn = SignalListModel::isBuiltIn(set);
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        const QString ref = SignalListModel::factsOf(file, builtIn, d).ref;
        if (ref.isEmpty()) {
            continue;
        }
        byRef.insert(ref, d.id);
        if (ref.startsWith(QLatin1String("key:")) &&
            d.id < ECUIO_USER_SIG_ID_BASE) {
            byRef.insert(QStringLiteral("id:%1").arg(d.id), d.id);
        }
    }
    for (const QVariant& cell : cells) {
        for (const QString& ref : cell.toStringList()) {
            const auto it = byRef.constFind(ref);
            if (it != byRef.constEnd()) {
                if (std::find(ids.begin(), ids.end(), *it) == ids.end()) {
                    ids.push_back(*it);
                }
                break;
            }
        }
    }
    return ids;
}

namespace {

// The default pick's activity line: its names, at most a dozen
QString pickText(const ecuio_signal_set_t* set,
                 const std::vector<uint16_t>& ids, int minSweepMs, bool polled)
{
    constexpr size_t kNamed = 12;
    QStringList names;
    for (uint16_t id : ids) {
        if (static_cast<size_t>(names.size()) == kNamed) {
            break;
        }
        for (uint32_t i = 0; i < set->count; ++i) {
            if (set->defs[i].id == id) {
                names << QString::fromUtf8(
                    set->defs[i].name,
                    static_cast<qsizetype>(
                        strnlen(set->defs[i].name, sizeof set->defs[i].name)));
                break;
            }
        }
    }
    QString list = names.join(QStringLiteral(", "));
    if (ids.size() > static_cast<size_t>(names.size())) {
        list += QStringLiteral(" and %1 more").arg(ids.size() - names.size());
    }
    const QString pace =
        minSweepMs > 0
            ? QStringLiteral(" (at least %1 ms between sweeps)").arg(minSweepMs)
            : QString();
    const QString count = signalCountText(static_cast<int>(ids.size()));
    return polled ? QStringLiteral("Picked %1%2; ?poll= asks only the polled "
                                   "ones among them: %3")
                        .arg(count, pace, list)
                  : QStringLiteral("Asking %1%2: %3").arg(count, pace, list);
}

// The obd2 defs a merge's BESIDE_BASE notes name: a part reads its own beside them
std::vector<uint16_t> besideBase(const ecuio_signal_file_t* merged)
{
    std::vector<uint16_t> ids;
    const ecuio_signal_set_t* set = ecuio_signal_file_set(merged);
    const uint32_t count = ecuio_signal_file_note_count(merged);
    for (uint32_t i = 0; set != nullptr && i < count; ++i) {
        ecuio_signal_note_t n{};
        n.struct_size = sizeof n;
        if (ecuio_signal_file_note(merged, i, &n) == ECUIO_OK &&
            n.kind == ECUIO_NOTE_BESIDE_BASE && n.base_def < set->count) {
            ids.push_back(set->defs[n.base_def].id);
        }
    }
    return ids;
}

} // namespace

bool SessionBridge::applyDefaultPick(ecuio_session_t* session, bool withTicks,
                                     bool again)
{
    m_pickDirty = false;
    if (session == nullptr || m_logActive) {
        return false;
    }
    ecuio_session_info_t info{};
    if (ecuio_session_get_info(session, &info) != ECUIO_OK ||
        (info.flags & ECUIO_SESSION_INFO_REQUESTS) == 0) {
        return false;
    }
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    std::vector<uint16_t> added = gaugeIds(set, fileOfSet(set), m_pickGauges);
    if (withTicks) {
        const std::vector<uint16_t> ticked = m_signalModel->tickedIds();
        added.insert(added.end(), ticked.begin(), ticked.end());
    }
    const bool pickOnly = (info.flags & ECUIO_SESSION_INFO_PICK_ONLY) != 0;
    const std::vector<uint16_t> ids = cli::default_pick(
        set, added, pickOnly, besideBase(togetherFileOf(set)));
    if (m_pickSet && !again && ids == m_pickIds &&
        m_minSweepMs == m_pickMinSweepMs) {
        return true;
    }
    m_pickIds = ids;
    m_pickMinSweepMs = m_minSweepMs;
    m_pickSet = true;
    m_pickDropped = 0;
    if (session == m_session) {
        for (uint32_t c = 0; c < m_channelCount; ++c) {
            m_pickDropped += m_supportReports[c].dropped;
        }
    }
    QString line;
    bool picked = false;
    const QString whole =
        pickOnly
            ? QStringLiteral("Asking no signal: ")
            : (m_pickPolled ? QStringLiteral("Asking every polled signal: ")
                            : QStringLiteral("Asking every signal: "));
    // Core takes a minimum only with a focus, so the whole set is unpaced;
    // a pick-only session asks nothing without one, so none is in question
    const QString unpaced =
        m_minSweepMs > 0 && !pickOnly
            ? QStringLiteral("; no minimum time between sweeps applies to it")
            : QString();
    if (ids.empty()) {
        (void)ecuio_session_set_focus(session, nullptr, 0);
        line = whole +
               (pickOnly ? QStringLiteral(
                               "this profile is asked only what is picked. "
                               "Tick signals on Live or place gauges")
                         : QStringLiteral("the profile ranks none")) +
               unpaced;
    } else {
        const ecuio_status_t rc = ecuio_session_set_focus_with(
            session, ids.data(), static_cast<uint32_t>(ids.size()),
            static_cast<uint32_t>(m_minSweepMs), ECUIO_FOCUS_DEFAULT_PICK);
        if (rc == ECUIO_OK) {
            line = pickText(set, ids, m_minSweepMs, m_pickPolled);
            picked = true;
        } else {
            const QString detail = QString::fromUtf8(ecuio_last_error_detail());
            (void)ecuio_session_set_focus(session, nullptr, 0);
            line =
                whole + (detail.isEmpty() ? failureText(rc) : detail) + unpaced;
        }
    }
    if (line == m_pickLine) {
        return true;
    }
    m_pickLine = line;
    m_pickLineCore = picked;
    // Before the apply, the line waits for the session's own lines
    if (session == m_session) {
        writePickLine();
    } else {
        m_pickLinePending = true;
    }
    return true;
}

void SessionBridge::pollDefaultPick()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        m_logActive) {
        return;
    }
    if (m_pickDirty) {
        applyDefaultPick(m_session, true, false);
        return;
    }
    if (!m_pickSet || m_pickIds.empty()) {
        return;
    }
    uint64_t dropped = 0;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        dropped += m_supportReports[c].dropped;
    }
    if (dropped > m_pickDropped) {
        applyDefaultPick(m_session, true, true);
    }
}

/* One channel's "ECU 0x…:" line: what its supported-PID bitmaps list,
 * which requests left the rotation, and why, and whether its Service 01
 * requests carry several PIDs (ADR-0039 Appendix A): packing starting or
 * ending brings a new line, which must not read like the last. Static, like
 * sessionInfoText, so test_bridge pins the wording without an adapter. */
QString BridgeFormats::refusalCode(uint8_t service, uint8_t nrc)
{
    return QStringLiteral("7F %1 %2").arg(hexByte(service), hexByte(nrc));
}

QString BridgeFormats::refusalText(uint8_t service, uint8_t nrc)
{
    const QString reply = refusalCode(service, nrc);
    const QString name = QString::fromLatin1(cli::nrc_name(nrc));
    return name.isEmpty() ? QStringLiteral("Refused: %1").arg(reply)
                          : QStringLiteral("Refused: %1 (%2)").arg(name, reply);
}

QString BridgeFormats::refusedUnnamedText(
    const std::vector<std::pair<uint32_t, uint32_t>>& more)
{
    QStringList parts;
    for (const auto& m : more) {
        parts << QStringLiteral("ECU %1 refused %2 more request%3")
                     .arg(hexId(m.first))
                     .arg(m.second)
                     .arg(m.second == 1 ? QString() : QStringLiteral("s"));
    }
    return parts.join(QStringLiteral("; "));
}

namespace {

QStringList moduleStateText(const ecuio_channel_support_t& s)
{
    if ((s.flags & ECUIO_SUPPORT_CLOSED) != 0) {
        return {QStringLiteral("closed: nothing read on it")};
    }
    if ((s.flags & ECUIO_SUPPORT_NO_ROOM) != 0) {
        return {QStringLiteral(
            "no room: every module the adapter listens to is in use")};
    }
    return {};
}

} // namespace

QString BridgeFormats::channelSupportText(const ecuio_channel_support_t& s)
{

    QStringList parts = moduleStateText(s);
    if ((s.flags & ECUIO_SUPPORT_PROBED) != 0) {
        // core's count of the data PIDs, and the last its bitmaps cover
        if ((s.flags & ECUIO_SUPPORT_COMPLETE) != 0) {
            parts << QStringLiteral("supports %1 PIDs").arg(s.pids_supported);
        } else if (s.ranges != 0) {
            parts << QStringLiteral(
                         "supports %1 PIDs up to %2, no answer past that")
                         .arg(s.pids_supported)
                         .arg(hexId(s.pids_through));
        } else {
            parts << QStringLiteral("no answer to the supported-PID query");
        }
    }

    QString requests = QStringLiteral("%1 request%2").arg(s.requests)
                           .arg(s.requests == 1 ? QString() : QStringLiteral("s"));
    if (s.dropped == 0) {
        requests += QStringLiteral(", none dropped");
    } else {
        QStringList gone;
        const uint32_t listed = std::min<uint32_t>(s.dropped, ECUIO_SUPPORT_MAX_DROPPED);
        for (uint32_t i = 0; i < listed; ++i) {
            const ecuio_dropped_request_t& d = s.dropped_list[i];
            QStringList key;
            for (uint8_t k = 0; k < d.length && k < sizeof(d.request); ++k) {
                key << hexByte(d.request[k]);
            }
            const QString nrc = QStringLiteral("7F %1 %2").arg(hexByte(d.request[0]),
                                                               hexByte(d.nrc));
            QString why;
            switch (d.reason) {
                case ECUIO_DROP_BITMAP:
                    why = QStringLiteral("not in its supported PIDs");
                    break;
                case ECUIO_DROP_NOT_SUPPORTED:
                    why = QStringLiteral("not supported: ") + nrc;
                    break;
                case ECUIO_DROP_SERVICE:
                    why = QStringLiteral("service not supported: ") + nrc;
                    break;
                case ECUIO_DROP_ACCESS:
                    // in the extended session, "another session" is wrong
                    why = (s.session == cli::kExtendedSession
                               ? QStringLiteral("needs security access, or a "
                                                "session Havne doesn't enter: ")
                               : QStringLiteral("needs security access or "
                                                "another session: ")) +
                          nrc;
                    break;
                default:
                    why = QStringLiteral("reason %1").arg(static_cast<uint>(d.reason));
                    break;
            }
            gone << key.join(QLatin1Char(' ')) + QStringLiteral(" (") + why + QLatin1Char(')');
        }
        if (s.dropped > listed) {
            gone << QStringLiteral("and %1 more").arg(s.dropped - listed);
        }
        requests += QStringLiteral(", %1 dropped: ").arg(s.dropped) +
                    gone.join(QStringLiteral(", "));
    }
    parts << requests;

    if ((s.flags & ECUIO_SUPPORT_UNPACKED) != 0) {
        parts << QStringLiteral("one PID per request, several went unanswered");
    } else if ((s.flags & ECUIO_SUPPORT_PACKED) != 0) {
        parts << QStringLiteral("several PIDs per request");
    }

    /* The DID scan (ADR-0020): how many DIDs it covers and what each
     * answer was; found ones are polled unless the table was full. */
    if (s.scan_total > 0) {
        QString scan = QStringLiteral("scan: %1 DID%2, %3 found")
                           .arg(s.scan_total)
                           .arg(s.scan_total == 1 ? QString() : QStringLiteral("s"))
                           .arg(s.scan_found);
        if (s.scan_polled < s.scan_found) {
            scan += QStringLiteral(" (%1 polled)").arg(s.scan_polled);
        }
        scan += QStringLiteral(", %1 refused").arg(s.scan_refused);
        if (s.scan_done < s.scan_total) {
            scan += QStringLiteral(", %1 unanswered").arg(s.scan_total - s.scan_done);
        }
        parts << scan;
    }
    if (s.session_asked != 0) {
        parts << (s.session == cli::kExtendedSession
                      ? QStringLiteral("extended session")
                      : QStringLiteral("default session"));
    }
    return QStringLiteral("ECU %1: ").arg(hexId(s.rx_id)) + parts.join(QStringLiteral("; "));
}

/* A module's session change (ADR-0060), one line per state the CLI's
 * session line reports; the refusal's code by name, as Live's refusals
 * are. Static, so test_bridge pins every state's words. */
QString BridgeFormats::channelSessionText(const ecuio_channel_support_t& s)
{
    const QString ecu = QStringLiteral("ECU %1: ").arg(hexId(s.rx_id));
    const QString name = QString::fromLatin1(cli::nrc_name(s.session_nrc));
    switch (s.session_asked != 0 ? s.session_state
                                 : ECUIO_SESSION_STATE_NOT_ASKED) {
    case ECUIO_SESSION_STATE_ENTERED:
        if (s.session_lost == 0) {
            return ecu +
                   QStringLiteral("reading in the extended session (50 03)");
        }
        return ecu +
               QStringLiteral("left the extended session and entered it again "
                              "(%1 %2 so far)")
                   .arg(s.session_lost)
                   .arg(s.session_lost == 1 ? QStringLiteral("time")
                                            : QStringLiteral("times"));
    case ECUIO_SESSION_STATE_REFUSED: {
        const QString code = refusalCode(0x10, s.session_nrc);
        return ecu + QStringLiteral(
                         "refused the extended session (%1): reading in the "
                         "default session")
                         .arg(name.isEmpty()
                                  ? code
                                  : name + QStringLiteral(", ") + code);
    }
    case ECUIO_SESSION_STATE_UNANSWERED:
        return ecu + QStringLiteral(
                         "no answer to 10 03: reading in the default session, "
                         "asked again when the link recovers");
    case ECUIO_SESSION_STATE_ABANDONED: {
        const QString code =
            QStringLiteral("code %1").arg(hexByte(s.session_nrc));
        return ecu +
               QStringLiteral("kept leaving the extended session (%1, %2 times "
                              "within %3 s): reading in the default session")
                   .arg(name.isEmpty() ? code
                                       : name + QStringLiteral(", ") + code)
                   .arg(ECUIO_SESSION_LOSSES_MAX)
                   .arg(ECUIO_SESSION_LOSS_WINDOW_MS / 1000);
    }
    default:
        return QString();
    }
}

QString BridgeFormats::sessionReturnedText(const ecuio_channel_support_t& s)
{
    if (s.session_asked == 0 || s.session_entries == 0) {
        return QString();
    }
    return QStringLiteral("ECU %1: returning to the default session (10 01)")
        .arg(hexId(s.rx_id));
}
