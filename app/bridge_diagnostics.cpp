// File/layer: app/bridge_diagnostics.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "bridgetext.h"
#include "clear_request.h"    /* cli/: one clear request over the ECUs */
#include "diagnostics_line.h" /* cli/: havne-cli's diagnostics line */
#include "enhanced_codes_line.h" /* cli/: and its enhanced_codes line */

#include <QByteArray>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace bridgetext;

namespace {

/* A clear core took none of, for the Log */
QString clearNotSent(ecuio_status_t rc)
{
    return rc == ECUIO_ERR_REPORT_CHANGED
               ? QStringLiteral("the report changed")
               : QString::fromUtf8(ecuio_status_string(rc));
}

} // namespace

/* What each channel's ECU said it is (ADR-0021), in the activity log
 * once the whole list has an outcome, and again when a recovery re-asks
 * the unanswered items and they settle. One line per ECU, so the VIN
 * and the calibration IDs land in the log and in a recording's
 * events.log together. The vehicle's VIN is taken as soon as it is in,
 * whatever else is still out. */
void SessionBridge::pollChannelIdentity()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_identity_t s{};
        if (ecuio_session_get_channel_identity(m_session, c, &s) != ECUIO_OK) {
            continue;
        }
        // The vehicle is the first channel's ECU's VIN (ADR-0027 section 3);
        // a recovery's re-asks settle on the same one and say nothing
        if (c == 0 && (s.flags & ECUIO_IDENTITY_VIN) != 0) {
            takeVin(s);
        }
        if ((s.flags & ECUIO_IDENTITY_COMPLETE) == 0 ||
            s.generation == m_identityGeneration[c]) {
            continue;
        }
        m_identityGeneration[c] = s.generation;
        emit logMessage(channelIdentityText(s));
    }
}

void SessionBridge::takeVin(const ecuio_channel_identity_t& s)
{
    char text[ECUIO_VIN_TEXT_LEN];
    const QString vin = ecuio_identity_vin(&s, text, sizeof text) == ECUIO_OK
                            ? QString::fromLatin1(text)
                            : QString();
    if (vin.isEmpty() || vin == m_vin) {
        return;
    }
    m_vin = vin;
    // What the VIN says of the car (ADR-0043)
    emit logMessage(QStringLiteral("VIN: ") +
                    vinSummary(vin, QDate::currentDate().year() + 1));
    emit vehicleIdentified();
    // the inspection's model year (ADR-0045)
    emit diagnosticsChanged();
}

/* Each channel's diagnostics report (ADR-0037), logged once per read
 * that completes: at connect, then once per Read again, which moves the
 * generation on. One line per ECU, so a recording's events.log keeps
 * the codes the car had when it was made. A clear (ADR-0038) moves the
 * generation with the codes unchanged, while it runs and when it ends
 * unsent or refused: that logs its own line, once per clear asked, and
 * no diagnostics line; after a 44 or no answer the read that follows
 * logs one. */
void SessionBridge::pollChannelDiagnostics()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    bool changed = m_diagnosticsShownCount != m_channelCount;
    bool clear_ended = false;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_diagnostics_t s{};
        if (ecuio_session_get_channel_diagnostics(m_session, c, &s) !=
            ECUIO_OK) {
            continue;
        }
        // The struct has no padding (core.h pins it); its doubles compare
        // by their bits, which is what a change test wants
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
        if (memcmp(&s, &m_diagnosticsShown[c], sizeof(s)) != 0) {
            m_diagnosticsShown[c] = s;
            changed = true;
        }
        bool clear_only = (s.flags & ECUIO_DIAG_CLEARING) != 0;
        if (m_clearAsked[c] && !clear_only &&
            cli::clear_settled(s, m_clearAskedGeneration[c])) {
            m_clearAsked[c] = false;
            clear_ended = true;
            emit logMessage(channelClearText(s, m_signalModel->unitSystem()));
            if (s.clear == ECUIO_CLEAR_MOVING ||
                s.clear == ECUIO_CLEAR_NO_VEHICLE_SPEED) {
                m_clearWaiting[c] = true;
                m_clearWaitGeneration[c] = s.generation;
                m_clearWaitKmh[c] = (s.flags & ECUIO_DIAG_CLEAR_KMH) != 0
                                        ? s.clear_kmh
                                        : UINT32_MAX;
            }
            clear_only = s.clear != ECUIO_CLEAR_CLEARED &&
                         s.clear != ECUIO_CLEAR_UNANSWERED;
            if (!clear_only) {
                queueModuleCodes(c);
            }
        }
        if ((s.flags & ECUIO_DIAG_COMPLETE) == 0 ||
            s.generation == m_diagnosticsGeneration[c]) {
            continue;
        }
        m_diagnosticsGeneration[c] = s.generation;
        if (!clear_only) {
            emit logMessage(channelDiagnosticsText(s));
        }
    }
    if (changed) {
        m_diagnosticsShownCount = m_channelCount;
        rebuildDiagnostics();
    }
    if (clear_ended) {
        emit clearMovingChanged();
    }
}

void SessionBridge::rebuildDiagnostics()
{
    m_diagnostics.clear();
    for (uint32_t c = 0; c < m_diagnosticsShownCount; ++c) {
        QVariantMap entry = diagnosticsEntry(m_diagnosticsShown[c],
                                             m_signalModel->unitSystem());
        entry[QStringLiteral("enhanced")] = moduleCodesEntry(
            m_codesShown[c], m_diagnosticsShown[c], sessionOffered());
        m_diagnostics << entry;
    }
    emit diagnosticsChanged();
}

bool BridgeFormats::clearTarget(const ecuio_channel_diagnostics_t& report)
{
    return (report.flags & ECUIO_DIAG_COMPLETE) != 0 &&
           (report.flags & ECUIO_DIAG_CLEARING) == 0;
}

bool BridgeFormats::nothingStored(const ecuio_channel_diagnostics_t& r)
{
    const auto empty = [&r](uint32_t list) {
        return r.lists[list].outcome == ECUIO_DIAG_ANSWERED &&
               r.lists[list].reported == 0 && r.lists[list].held == 0;
    };
    return (r.flags & ECUIO_DIAG_READINESS) != 0 &&
           (r.flags & ECUIO_DIAG_MIL_ON) == 0 && r.dtc_count == 0 &&
           empty(ECUIO_DIAG_STORED) && empty(ECUIO_DIAG_PENDING) &&
           (r.flags & ECUIO_DIAG_FREEZE_FRAME) == 0;
}

bool SessionBridge::clearFindsNothing() const
{
    bool any = false;
    for (uint32_t c = 0; c < m_diagnosticsShownCount; ++c) {
        if (!clearTarget(m_diagnosticsShown[c])) {
            continue;
        }
        if (!nothingStored(m_diagnosticsShown[c])) {
            return false;
        }
        any = true;
    }
    return any;
}

QStringList SessionBridge::clearTargets() const
{
    QStringList ecus;
    for (uint32_t c = 0; c < m_diagnosticsShownCount; ++c) {
        if (clearTarget(m_diagnosticsShown[c])) {
            ecus << hexId(m_diagnosticsShown[c].rx_id);
        }
    }
    return ecus;
}

/* ADR-0038: what a clear erases stays on record, so the reports go to a
 * file of their own before anything is sent, whether or not the session
 * records; each ECU is then cleared at the generation that file holds,
 * so a report that moved on since is refused unsent */
QString SessionBridge::clearDiagnostics()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return {};
    }
    ecuio_channel_diagnostics_t reports[ECUIO_MAX_CHANNELS];
    uint32_t channels[ECUIO_MAX_CHANNELS];
    uint32_t n = 0;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_diagnostics_t s{};
        if (ecuio_session_get_channel_diagnostics(m_session, c, &s) ==
                ECUIO_OK &&
            clearTarget(s)) {
            reports[n] = s;
            channels[n] = c;
            ++n;
        }
    }
    if (n == 0) {
        emit logMessage(
            QStringLiteral("Clear not sent: no ECU has a completed read"));
        return {};
    }

    const auto refuse = [this](const QString& why) {
        emit logMessage(
            QStringLiteral("Clear not sent: the report couldn't be saved (%1)")
                .arg(why));
        return QString();
    };
    const QString root = recordingsRoot();
    if (root.isEmpty()) {
        return refuse(QStringLiteral("no Documents folder"));
    }
    // The picked vehicle names an adapter's file only, as a recording's
    // folder (ADR-0027 section 1)
    const bool adapter = m_currentUri.startsWith(QStringLiteral("j2534://"),
                                                 Qt::CaseInsensitive) ||
                         m_currentUri.startsWith(QStringLiteral("elm327://"),
                                                 Qt::CaseInsensitive);
    const QString label = adapter ? folderLabel(m_vehicleLabel) : QString();
    const QString base =
        QStringLiteral("diagnostics-") + QDateTime::currentDateTime().toString(
                                             QStringLiteral("yyyyMMdd-HHmmss"));
    QString path;
    for (int k = 1; k <= 9 && path.isEmpty(); ++k) {
        const QString name =
            base + (k > 1 ? QStringLiteral("-%1").arg(k) : QString()) +
            (label.isEmpty() ? QString() : QLatin1Char(' ') + label) +
            QStringLiteral(".jsonl");
        if (!QFileInfo::exists(root + QLatin1Char('/') + name)) {
            path = root + QLatin1Char('/') + name;
        }
    }
    if (path.isEmpty()) {
        return refuse(
            QStringLiteral("nine files already have this second's name"));
    }
    /* No folder, no file: either way nothing is sent */
    const bool folder = QDir().mkpath(root);
    QSaveFile file(path);
    const QByteArray text = clearFileText(reports, channels, n);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() ||
        !file.commit()) {
        return refuse(folder ? file.errorString()
                             : QStringLiteral("can't create %1")
                                   .arg(documentsRelative(root)));
    }
    const QString shown = documentsRelative(path);
    emit logMessage(
        QStringLiteral("Diagnostics saved before the clear: %1").arg(shown));
    m_clearSavedTo = shown;
    std::fill(std::begin(m_clearWaiting), std::end(m_clearWaiting), false);
    emit clearMovingChanged();

    /* One request: the engine and vehicle speed judged once for every ECU */
    std::vector<ecuio_clear_target_t> targets(n);
    for (uint32_t i = 0; i < n; ++i) {
        targets[i] = {channels[i], reports[i].generation, {0, 0}};
    }
    const std::vector<ecuio_status_t> rc =
        cli::ask_clears(m_session, targets, ECUIO_CLEAR_ALLOW_RUNNING);
    for (uint32_t i = 0; i < n; ++i) {
        if (rc[i] == ECUIO_OK) {
            m_clearAsked[channels[i]] = true;
            m_clearAskedGeneration[channels[i]] = reports[i].generation;
        } else {
            emit logMessage(
                QStringLiteral("ECU %1 clear: not sent: %2")
                    .arg(hexId(reports[i].rx_id), clearNotSent(rc[i])));
        }
    }
    return shown;
}

QStringList SessionBridge::clearMoving() const
{
    QStringList ecus;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (m_clearAsked[c]) {
            return {}; /* asked once all have ended */
        }
    }
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (m_clearWaiting[c]) {
            ecus << hexId(m_diagnosticsShown[c].rx_id);
        }
    }
    return ecus;
}

QString SessionBridge::clearMovingSpeed() const
{
    uint32_t kmh = UINT32_MAX;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (m_clearWaiting[c] && m_clearWaitKmh[c] != UINT32_MAX &&
            (kmh == UINT32_MAX || m_clearWaitKmh[c] > kmh)) {
            kmh = m_clearWaitKmh[c];
        }
    }
    if (kmh == UINT32_MAX) {
        return {};
    }
    return QStringLiteral("%1 %2")
        .arg(qRound(displayValue(QStringLiteral("km/h"), kmh)))
        .arg(displayUnit(QStringLiteral("km/h")));
}

void SessionBridge::clearDiagnosticsMoving()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        clearMoving().isEmpty()) {
        return;
    }
    emit logMessage(QStringLiteral("Clearing while moving; the report was "
                                   "saved before the clear: %1")
                        .arg(m_clearSavedTo));
    std::vector<ecuio_clear_target_t> targets;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (m_clearWaiting[c]) {
            m_clearWaiting[c] = false;
            targets.push_back({c, m_clearWaitGeneration[c], {0, 0}});
        }
    }
    const std::vector<ecuio_status_t> rc =
        cli::ask_clears(m_session, targets,
                        ECUIO_CLEAR_ALLOW_RUNNING | ECUIO_CLEAR_ALLOW_MOVING);
    for (size_t k = 0; k < targets.size(); ++k) {
        const uint32_t c = targets[k].channel;
        if (rc[k] == ECUIO_OK) {
            m_clearAsked[c] = true;
            m_clearAskedGeneration[c] = targets[k].generation;
        } else {
            emit logMessage(QStringLiteral("ECU %1 clear: not sent: %2")
                                .arg(hexId(m_diagnosticsShown[c].rx_id),
                                     clearNotSent(rc[k])));
        }
    }
    emit clearMovingChanged();
}

void SessionBridge::cancelClearMoving()
{
    const QStringList ecus = clearMoving();
    if (ecus.isEmpty()) {
        return;
    }
    for (bool& waiting : m_clearWaiting) {
        waiting = false;
    }
    emit logMessage(QStringLiteral("Clear while moving not sent: %1")
                        .arg(ecus.join(QStringLiteral(", "))));
    emit clearMovingChanged();
}

QByteArray
BridgeFormats::diagnosticsFileText(const ecuio_channel_diagnostics_t* reports,
                                   uint32_t count,
                                   const ecuio_module_codes_t* codes)
{
    QByteArray out;
    char buf[8192];
    cli::Line start(buf, sizeof buf);
    start.raw("{\"type\":\"start\"");
    start.key("schema");
    start.u64(1);
    start.key("core");
    start.string(ecuio_version_string(), SIZE_MAX);
    start.ch('}');
    if (start.finish()) {
        out.append(start.data(), static_cast<qsizetype>(start.size()));
    }
    for (uint32_t i = 0; i < count; ++i) {
        cli::Line l(buf, sizeof buf);
        cli::diagnostics_line(l, reports[i]);
        if (l.finish()) {
            out.append(l.data(), static_cast<qsizetype>(l.size()));
        }
        if (codes != nullptr &&
            (codes[i].flags & ECUIO_MODULE_CODES_COMPLETE) != 0 &&
            codes[i].read_id != 0) {
            std::vector<char> big(cli::kEnhancedCodesLineBytes);
            cli::Line e(big.data(), big.size());
            cli::enhanced_codes_line(e, codes[i], &reports[i]);
            if (e.finish()) {
                out.append(e.data(), static_cast<qsizetype>(e.size()));
            }
        }
    }
    return out;
}
QString SessionBridge::vinKey(const QString& salt) const
{
    if (m_vin.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(
        QCryptographicHash::hash((salt + m_vin).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
}

QString BridgeFormats::vinSummary(const QString& vin, int latestModelYear)
{
    const QByteArray text = vin.toLatin1();
    ecuio_vin_info_t v{};
    if (vin.size() != ECUIO_VIN_LEN ||
        ecuio_vin_decode(text.constData(),
                         static_cast<uint32_t>(std::max(latestModelYear, 0)),
                         &v) != ECUIO_OK) {
        return QStringLiteral(
            "not a VIN: 49 CFR 565 takes 17 digits and capital "
            "letters, never I, O or Q");
    }
    QStringList parts;
    if (v.model_year == 0) {
        parts << QStringLiteral("no model year (position 10 is %1)")
                     .arg(vin.at(9));
    } else if ((v.flags & ECUIO_VIN_YEAR_EARLIER) != 0) {
        parts << QStringLiteral("model year %1 (not %2, which is still ahead)")
                     .arg(v.model_year)
                     .arg(v.model_year + 30);
    } else {
        parts << QStringLiteral("model year %1").arg(v.model_year);
    }
    const QString make = QString::fromLatin1(ecuio_vin_make(v.wmi));
    const QString wmi = QString::fromLatin1(v.wmi);
    parts << (make.isEmpty()
                  ? QStringLiteral("manufacturer code %1 isn't in NHTSA's list")
                        .arg(wmi)
                  : QStringLiteral("%1 (manufacturer code %2)").arg(make, wmi));
    parts << ((v.flags & ECUIO_VIN_CHECK_MATCHES) != 0
                  ? QStringLiteral("check digit matches")
                  : QStringLiteral(
                        "check digit %1 isn't the %2 it works out to (North "
                        "America requires it; other markets may not)")
                        .arg(vin.at(8))
                        .arg(QChar::fromLatin1(v.check_digit)));
    return parts.join(QStringLiteral("; "));
}

QString SessionBridge::describeVin(const QString& typed) const
{
    QString vin = typed.toUpper();
    vin.remove(QLatin1Char(' '));
    if (vin.isEmpty()) {
        return {};
    }
    return vinSummary(vin, QDate::currentDate().year() + 1);
}

QString SessionBridge::vinSource() const
{
    return QString::fromLatin1(ecuio_vin_source());
}

QString SessionBridge::describeDtc(const QString& typed) const
{
    QString text = typed.toUpper();
    text.remove(QLatin1Char(' '));
    if (text.isEmpty()) {
        return {};
    }
    const QByteArray latin = text.toLatin1();
    uint16_t code = 0;
    if (text.size() != 5 ||
        ecuio_dtc_parse(latin.constData(), &code) != ECUIO_OK) {
        return QStringLiteral("not a trouble code: a letter P, C, B or U, then "
                              "four hex digits, the first 0-3");
    }
    const QString title = QString::fromLatin1(ecuio_dtc_title(code));
    return title.isEmpty()
               ? QStringLiteral("%1: no title here (Havne names SAE's generic "
                                "powertrain and network codes; a manufacturer "
                                "defines its own)")
                     .arg(text)
               : QStringLiteral("%1: %2").arg(text, title);
}
