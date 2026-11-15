// File/layer: app/bridge_diagnostics.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "bridgetext.h"
#include "diagnostics_line.h" /* cli/: havne-cli's diagnostics line */

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

using namespace bridgetext;

namespace {

// A clear's vehicle speed in the unit system, converted by core
// (ADR-0022): "48 km/h", "30 mph"; "" if core can't convert it.
QString clearSpeedText(uint32_t kmh, uint32_t units)
{
    ecuio_display_value_t out{};
    if (ecuio_display_convert("km/h", 0, kmh, units, &out) !=
        ECUIO_DISPLAY_OK) {
        return {};
    }
    return QStringLiteral("%1 %2")
        .arg(qRound(out.value))
        .arg(QString::fromUtf8(out.unit));
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
        if (m_clearAsked[c] && !clear_only && s.clear != ECUIO_CLEAR_NONE) {
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
        m_diagnostics << diagnosticsEntry(m_diagnosticsShown[c],
                                          m_signalModel->unitSystem());
    }
    emit diagnosticsChanged();
}

void SessionBridge::readDiagnostics()
{
    if (m_connected.load(std::memory_order_acquire) && m_session != nullptr) {
        (void)ecuio_session_read_diagnostics(m_session);
    }
}

bool SessionBridge::clearTarget(const ecuio_channel_diagnostics_t& report)
{
    return (report.flags & ECUIO_DIAG_COMPLETE) != 0 &&
           (report.flags & ECUIO_DIAG_CLEARING) == 0;
}

bool SessionBridge::nothingStored(const ecuio_channel_diagnostics_t& r)
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

QVariantMap SessionBridge::inspection() const
{
    uint32_t year = 0;
    ecuio_vin_info_t v{};
    const QByteArray vin = m_vin.toLatin1();
    if (!m_vin.isEmpty() &&
        ecuio_vin_decode(vin.constData(),
                         static_cast<uint32_t>(QDate::currentDate().year() + 1),
                         &v) == ECUIO_OK) {
        year = v.model_year;
    }
    return inspectionEntry(m_diagnosticsShown, m_diagnosticsShownCount, year);
}

QVariantMap
SessionBridge::inspectionEntry(const ecuio_channel_diagnostics_t* reports,
                               uint32_t count, uint32_t modelYear)
{
    QVariantMap m;
    ecuio_inspection_t i{};
    if (ecuio_diag_inspect(reports, count, modelYear, &i) != ECUIO_OK) {
        i.verdict = ECUIO_INSPECTION_UNKNOWN;
    }
    QStringList names;
    for (uint8_t id = 0; id < 16; ++id) {
        if (((i.incomplete_ids >> id) & 1u) != 0) {
            names << QString::fromLatin1(
                ecuio_monitor_short_name(id, i.diag_flags));
        }
    }
    const QString incomplete =
        (i.incomplete == 1
             ? QStringLiteral("1 monitor incomplete")
             : QStringLiteral("%1 monitors incomplete").arg(i.incomplete)) +
        QStringLiteral(" (%1)").arg(names.join(QStringLiteral(", ")));
    QString verdict;
    QString text;
    switch (i.verdict) {
    case ECUIO_INSPECTION_READY:
        verdict = QStringLiteral("ready");
        if (i.incomplete == 0) {
            text =
                QStringLiteral("Ready for an emissions inspection: every "
                               "monitor complete, the check-engine light off.");
        } else if (modelYear == 0) {
            text =
                QStringLiteral("Ready for an emissions inspection: %1, which "
                               "a car of any model year may have.")
                    .arg(incomplete);
        } else {
            text =
                QStringLiteral("Ready for an emissions inspection: %1, which "
                               "a model year %2 car may have.")
                    .arg(incomplete)
                    .arg(modelYear);
        }
        break;
    case ECUIO_INSPECTION_NOT_READY:
        verdict = QStringLiteral("not_ready");
        text =
            (modelYear == 0
                 ? QStringLiteral("Not ready for an emissions inspection: %1, "
                                  "and no model year may have more than 2.")
                       .arg(incomplete)
                 : QStringLiteral(
                       "Not ready for an emissions inspection: %1, "
                       "and a model year %2 car may have %3 at most.")
                       .arg(incomplete)
                       .arg(modelYear)
                       .arg(i.allowed)) +
            QStringLiteral(" A station would send it away to be driven until "
                           "they complete.");
        break;
    case ECUIO_INSPECTION_MIL_ON:
        verdict = QStringLiteral("mil_on");
        text = QStringLiteral("Would fail an emissions inspection: an ECU "
                              "commands the check-engine light on.");
        break;
    case ECUIO_INSPECTION_YEAR_NEEDED:
        verdict = QStringLiteral("year_needed");
        text =
            QStringLiteral("%1: ready for an emissions inspection if the car "
                           "is model year 1996-2000, not if it's 2001 or "
                           "newer, and no VIN gave its year.")
                .arg(incomplete);
        break;
    case ECUIO_INSPECTION_NOT_COVERED:
        verdict = QStringLiteral("not_covered");
        text = QStringLiteral("Model year %1: the federal inspection rule "
                              "covers 1996 and newer.")
                   .arg(modelYear);
        break;
    default:
        break;
    }
    m[QStringLiteral("verdict")] = verdict;
    m[QStringLiteral("text")] = text;
    m[QStringLiteral("rule")] = QStringLiteral(
        "By the federal rule, 40 CFR 85.2222, from the monitors and "
        "the VIN's model year. A state's program sets its own, "
        "and some are stricter.");
    return m;
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
    ecuio_channel_diagnostics_t reports[ECUIO_SESSION_INFO_MAX_CHANNELS];
    uint32_t channels[ECUIO_SESSION_INFO_MAX_CHANNELS];
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
    const QByteArray text = diagnosticsFileText(reports, n);
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
    for (bool& waiting : m_clearWaiting) {
        waiting = false;
    }
    emit clearMovingChanged();

    for (uint32_t i = 0; i < n; ++i) {
        const ecuio_status_t rc = ecuio_session_clear_diagnostics_with(
            m_session, channels[i], reports[i].generation,
            ECUIO_CLEAR_ALLOW_RUNNING);
        if (rc == ECUIO_OK) {
            m_clearAsked[channels[i]] = true;
        } else {
            emit logMessage(
                QStringLiteral("ECU %1 clear: not sent: %2")
                    .arg(hexId(reports[i].rx_id),
                         rc == ECUIO_ERR_REPORT_CHANGED
                             ? QStringLiteral("the report changed")
                             : QString::fromUtf8(ecuio_status_string(rc))));
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
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (!m_clearWaiting[c]) {
            continue;
        }
        m_clearWaiting[c] = false;
        const ecuio_status_t rc = ecuio_session_clear_diagnostics_with(
            m_session, c, m_clearWaitGeneration[c],
            ECUIO_CLEAR_ALLOW_RUNNING | ECUIO_CLEAR_ALLOW_MOVING);
        if (rc == ECUIO_OK) {
            m_clearAsked[c] = true;
        } else {
            emit logMessage(
                QStringLiteral("ECU %1 clear: not sent: %2")
                    .arg(hexId(m_diagnosticsShown[c].rx_id),
                         rc == ECUIO_ERR_REPORT_CHANGED
                             ? QStringLiteral("the report changed")
                             : QString::fromUtf8(ecuio_status_string(rc))));
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
SessionBridge::diagnosticsFileText(const ecuio_channel_diagnostics_t* reports,
                                   uint32_t count)
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
    }
    return out;
}

QVariantMap
SessionBridge::diagnosticsEntry(const ecuio_channel_diagnostics_t& s,
                                uint32_t units)
{
    const auto outcome = [](uint8_t o) {
        switch (o) {
        case ECUIO_DIAG_ANSWERED:
            return QStringLiteral("answered");
        case ECUIO_DIAG_REFUSED:
            return QStringLiteral("refused");
        case ECUIO_DIAG_UNANSWERED:
            return QStringLiteral("unanswered");
        case ECUIO_DIAG_UNSUPPORTED:
            return QStringLiteral("unsupported");
        default:
            return QStringLiteral("unasked");
        }
    };
    const auto nrc = [](uint8_t o, uint8_t service, uint8_t code) {
        return o == ECUIO_DIAG_REFUSED
                   ? QStringLiteral("7F %1 %2")
                         .arg(hexByte(service), hexByte(code))
                   : QString();
    };
    const auto dtc = [](uint16_t code) {
        char text[8] = {};
        (void)ecuio_dtc_format(code, text, sizeof(text));
        return QString::fromLatin1(text);
    };

    QVariantMap m;
    m[QStringLiteral("ecu")] = hexId(s.rx_id);
    m[QStringLiteral("asked")] = (s.flags & ECUIO_DIAG_ASKED) != 0;
    m[QStringLiteral("complete")] = (s.flags & ECUIO_DIAG_COMPLETE) != 0;
    m[QStringLiteral("readiness")] = outcome(s.readiness);
    m[QStringLiteral("readinessNrc")] = nrc(s.readiness, 0x01, s.readiness_nrc);
    m[QStringLiteral("mil")] = (s.flags & ECUIO_DIAG_MIL_ON) != 0;
    m[QStringLiteral("dtcCount")] = static_cast<int>(s.dtc_count);
    m[QStringLiteral("compression")] = (s.flags & ECUIO_DIAG_COMPRESSION) != 0;
    m[QStringLiteral("cycle")] = outcome(s.cycle);

    static const char* const kKeys[ECUIO_DIAG_LISTS] = {"stored", "pending",
                                                        "permanent"};
    static const uint8_t kServices[ECUIO_DIAG_LISTS] = {0x03, 0x07, 0x0A};
    QVariantList lists;
    for (uint32_t l = 0; l < ECUIO_DIAG_LISTS; ++l) {
        const ecuio_dtc_list_t& list = s.lists[l];
        const uint8_t held = std::min<uint8_t>(list.held, ECUIO_DIAG_MAX_DTCS);
        QStringList codes;
        QStringList titles; // each code's (ADR-0044), "" with none
        for (uint8_t i = 0; i < held; ++i) {
            codes << dtc(list.codes[i]);
            titles << QString::fromLatin1(ecuio_dtc_title(list.codes[i]));
        }
        QVariantMap e;
        e[QStringLiteral("key")] = QString::fromLatin1(kKeys[l]);
        e[QStringLiteral("outcome")] = outcome(list.outcome);
        e[QStringLiteral("nrc")] = nrc(list.outcome, kServices[l], list.nrc);
        e[QStringLiteral("codes")] = codes;
        e[QStringLiteral("titles")] = titles;
        e[QStringLiteral("more")] =
            list.reported > held ? list.reported - held : 0;
        lists << e;
    }
    m[QStringLiteral("lists")] = lists;

    QVariantList monitors;
    const uint8_t n =
        std::min<uint8_t>(s.monitor_count, ECUIO_DIAG_MAX_MONITORS);
    for (uint8_t i = 0; i < n; ++i) {
        QVariantMap e;
        e[QStringLiteral("id")] = static_cast<int>(s.monitors[i].id);
        e[QStringLiteral("name")] =
            QString::fromLatin1(ecuio_monitor_name(s.monitors[i].id, s.flags));
        e[QStringLiteral("complete")] =
            (s.monitors[i].flags & ECUIO_MONITOR_COMPLETE) != 0;
        e[QStringLiteral("cycleEnabled")] =
            (s.monitors[i].flags & ECUIO_MONITOR_CYCLE_ENABLED) != 0;
        e[QStringLiteral("cycleComplete")] =
            (s.monitors[i].flags & ECUIO_MONITOR_CYCLE_COMPLETE) != 0;
        monitors << e;
    }
    m[QStringLiteral("monitors")] = monitors;

    /* Each freeze-frame value by the obd2 def that decoded it (ADR-0037
     * section 4), in the user's units, as Live shows a live one */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    QVariantList values;
    const uint8_t fn = std::min<uint8_t>(s.freeze_count, ECUIO_DIAG_MAX_FREEZE);
    for (uint8_t i = 0; i < fn; ++i) {
        const ecuio_freeze_value_t& f = s.freeze_values[i];
        for (uint32_t k = 0; k < obd2->count; ++k) {
            const ecuio_signal_def_t& def = obd2->defs[k];
            if (def.id != f.def_id) {
                continue;
            }
            ecuio_display_value_t out{};
            if (ecuio_display_convert(def.unit, def.decimal_places, f.value,
                                      units, &out) != ECUIO_DISPLAY_OK) {
                out = {};
                out.value = f.value;
                qstrncpy(out.unit, def.unit, sizeof(out.unit));
                out.decimal_places = def.decimal_places;
            }
            QVariantMap e;
            e[QStringLiteral("name")] = QString::fromUtf8(def.name);
            e[QStringLiteral("value")] = out.value;
            e[QStringLiteral("unit")] = QString::fromUtf8(out.unit);
            e[QStringLiteral("decimals")] =
                static_cast<int>(out.decimal_places);
            values << e;
            break;
        }
    }
    QVariantMap freeze;
    freeze[QStringLiteral("outcome")] = outcome(s.freeze);
    freeze[QStringLiteral("nrc")] = nrc(s.freeze, 0x02, s.freeze_nrc);
    freeze[QStringLiteral("stored")] = (s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0;
    freeze[QStringLiteral("code")] = (s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0
                                         ? dtc(s.freeze_dtc)
                                         : QString();
    freeze[QStringLiteral("title")] =
        (s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0
            ? QString::fromLatin1(ecuio_dtc_title(s.freeze_dtc))
            : QString();
    freeze[QStringLiteral("values")] = values;
    m[QStringLiteral("freeze")] = freeze;

    /* The last clear (ADR-0038) and whether one runs */
    m[QStringLiteral("clearing")] = (s.flags & ECUIO_DIAG_CLEARING) != 0;
    QString clear;
    QString clearNrc;
    switch (s.clear) {
    case ECUIO_CLEAR_CLEARED:
        clear = QStringLiteral("cleared");
        break;
    case ECUIO_CLEAR_REFUSED:
        clear = QStringLiteral("refused");
        clearNrc = QStringLiteral("7F 04 %1").arg(hexByte(s.clear_nrc));
        break;
    case ECUIO_CLEAR_UNANSWERED:
        clear = QStringLiteral("unanswered");
        break;
    case ECUIO_CLEAR_ENGINE_RUNNING:
        clear = QStringLiteral("engineRunning");
        break;
    case ECUIO_CLEAR_NO_ENGINE_SPEED:
        clear = QStringLiteral("noEngineSpeed");
        if (s.clear_nrc != 0) {
            clearNrc = QStringLiteral("7F 01 %1").arg(hexByte(s.clear_nrc));
        }
        break;
    case ECUIO_CLEAR_REPORT_CHANGED:
        clear = QStringLiteral("reportChanged");
        break;
    case ECUIO_CLEAR_MOVING:
        clear = QStringLiteral("moving");
        break;
    case ECUIO_CLEAR_NO_VEHICLE_SPEED:
        clear = QStringLiteral("noVehicleSpeed");
        if (s.clear_nrc != 0) {
            clearNrc = QStringLiteral("7F 01 %1").arg(hexByte(s.clear_nrc));
        }
        break;
    default:
        clear = QStringLiteral("none");
        break;
    }
    m[QStringLiteral("clear")] = clear;
    m[QStringLiteral("clearNrc")] = clearNrc;
    m[QStringLiteral("clearRpm")] = static_cast<int>(s.clear_rpm);
    /* What the clear read: the engine turning (a 7F 04 22 then means it
     * won't clear running), and the vehicle speed in the unit system */
    m[QStringLiteral("clearRunning")] =
        (s.flags & ECUIO_DIAG_CLEAR_RPM) != 0 && s.clear_rpm > 0;
    m[QStringLiteral("clearSpeed")] = (s.flags & ECUIO_DIAG_CLEAR_KMH) != 0
                                          ? clearSpeedText(s.clear_kmh, units)
                                          : QString();
    return m;
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

QString SessionBridge::vinSummary(const QString& vin, int latestModelYear)
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

/* One channel's "ECU 0x… identity:" line: each answered item by its
 * name and text, then the refused requests with their codes, then how
 * many went unanswered. Static, like channelSupportText, so test_bridge
 * pins the wording without an adapter. */
QString SessionBridge::channelIdentityText(const ecuio_channel_identity_t& s)
{

    QStringList parts;
    QStringList refused;
    uint32_t unanswered = 0;
    const uint32_t items = std::min<uint32_t>(s.items, ECUIO_IDENTITY_MAX_ITEMS);
    for (uint32_t i = 0; i < items; ++i) {
        const ecuio_identity_item_t& it = s.list[i];
        QStringList key;
        for (uint8_t k = 0; k < it.length && k < sizeof(it.request); ++k) {
            key << hexByte(it.request[k]);
        }
        const char* name = ecuio_identity_request_name(it.request, it.length);
        const QString label = (*name != '\0') ? QString::fromLatin1(name)
                                              : key.join(QLatin1Char(' '));
        switch (it.outcome) {
            case ECUIO_IDENTITY_ANSWERED: {
                const uint8_t n = std::min<uint8_t>(it.text_length,
                                                    ECUIO_IDENTITY_TEXT_LEN - 1);
                parts << label + QLatin1Char(' ') +
                             (n > 0 ? QString::fromLatin1(it.text, n)
                                    : QStringLiteral("(empty)"));
                break;
            }
            case ECUIO_IDENTITY_REFUSED:
                refused << key.join(QLatin1Char(' ')) +
                               QStringLiteral(" (7F %1 %2)").arg(hexByte(it.request[0]),
                                                                 hexByte(it.nrc));
                break;
            case ECUIO_IDENTITY_UNANSWERED:
                ++unanswered;
                break;
            default:
                break;
        }
    }
    if (!refused.isEmpty()) {
        parts << QStringLiteral("%1 refused: ").arg(refused.size()) +
                     refused.join(QStringLiteral(", "));
    }
    if (unanswered > 0) {
        parts << QStringLiteral("%1 unanswered").arg(unanswered);
    }
    if (parts.isEmpty()) {
        parts << QStringLiteral("nothing asked");
    }
    return QStringLiteral("ECU %1 identity: ").arg(hexId(s.rx_id)) +
           parts.join(QStringLiteral("; "));
}

/* One channel's "ECU 0x… clear:" line (ADR-0038): what became of the
 * clear its report names, a speed in the unit system the page shows */
QString SessionBridge::channelClearText(const ecuio_channel_diagnostics_t& s,
                                        uint32_t units)
{
    QString what;
    const bool rpm_read = (s.flags & ECUIO_DIAG_CLEAR_RPM) != 0;
    const bool kmh_read = (s.flags & ECUIO_DIAG_CLEAR_KMH) != 0;
    switch (s.clear) {
    case ECUIO_CLEAR_CLEARED:
        what = QStringLiteral("cleared");
        /* The speeds it was cleared at, when the user's word let it run */
        if (kmh_read) {
            what +=
                QStringLiteral(" (%1, %2)")
                    .arg(rpm_read ? QStringLiteral("%1 rpm").arg(s.clear_rpm)
                                  : QStringLiteral("engine speed unknown"))
                    .arg(clearSpeedText(s.clear_kmh, units));
        }
        break;
    case ECUIO_CLEAR_REFUSED:
        what = QStringLiteral("refused (7F 04 %1)").arg(hexByte(s.clear_nrc));
        /* J1979: an ECU that can't clear with the engine running answers
         * conditionsNotCorrect */
        if (s.clear_nrc == 0x22 && rpm_read && s.clear_rpm > 0) {
            what += QStringLiteral(": this ECU won't clear with the engine "
                                   "running: stop and turn it off, then try "
                                   "again");
        }
        break;
    case ECUIO_CLEAR_UNANSWERED:
        what = QStringLiteral("no answer");
        break;
    case ECUIO_CLEAR_ENGINE_RUNNING:
        what = QStringLiteral("not sent: engine running (%1 rpm)")
                   .arg(s.clear_rpm);
        break;
    case ECUIO_CLEAR_NO_ENGINE_SPEED:
        what = s.clear_nrc != 0
                   ? QStringLiteral("not sent: no engine speed (7F 01 %1)")
                         .arg(hexByte(s.clear_nrc))
                   : QStringLiteral("not sent: no engine speed");
        break;
    case ECUIO_CLEAR_REPORT_CHANGED:
        what = QStringLiteral("not sent: the report changed");
        break;
    case ECUIO_CLEAR_MOVING:
        what = QStringLiteral("not sent: moving (%1)")
                   .arg(clearSpeedText(s.clear_kmh, units));
        break;
    case ECUIO_CLEAR_NO_VEHICLE_SPEED:
        what = s.clear_nrc != 0
                   ? QStringLiteral("not sent: no vehicle speed (7F 01 %1)")
                         .arg(hexByte(s.clear_nrc))
                   : QStringLiteral("not sent: no vehicle speed");
        break;
    default:
        what = QStringLiteral("not asked");
        break;
    }
    return QStringLiteral("ECU %1 clear: %2").arg(hexId(s.rx_id), what);
}

/* One channel's "ECU 0x… diagnostics:" line: the MIL, each code list,
 * readiness (the incomplete monitors by their short names) and the
 * freeze frame's code, each as answered, refused with its code or
 * unanswered; an item never asked is left out. Static, like
 * channelIdentityText, so test_bridge pins the wording without an
 * adapter. */
QString
SessionBridge::channelDiagnosticsText(const ecuio_channel_diagnostics_t& s)
{
    const auto dtc = [](uint16_t code) {
        char text[8] = {};
        (void)ecuio_dtc_format(code, text, sizeof(text));
        return QString::fromLatin1(text);
    };
    const auto refused = [](uint8_t service, uint8_t nrc) {
        return QStringLiteral(" refused (7F %1 %2)")
            .arg(hexByte(service), hexByte(nrc));
    };
    QStringList parts;
    if (s.readiness == ECUIO_DIAG_ANSWERED) {
        parts << ((s.flags & ECUIO_DIAG_MIL_ON) != 0
                      ? QStringLiteral("MIL on")
                      : QStringLiteral("MIL off"));
    }

    static const char* const kNames[ECUIO_DIAG_LISTS] = {"stored", "pending",
                                                         "permanent"};
    static const uint8_t kServices[ECUIO_DIAG_LISTS] = {0x03, 0x07, 0x0A};
    for (uint32_t l = 0; l < ECUIO_DIAG_LISTS; ++l) {
        const ecuio_dtc_list_t& list = s.lists[l];
        const QString name = QString::fromLatin1(kNames[l]);
        switch (list.outcome) {
        case ECUIO_DIAG_ANSWERED: {
            QStringList codes;
            const uint8_t held =
                std::min<uint8_t>(list.held, ECUIO_DIAG_MAX_DTCS);
            for (uint8_t i = 0; i < held; ++i) {
                codes << dtc(list.codes[i]);
            }
            if (codes.isEmpty()) {
                codes << QStringLiteral("none");
            }
            QString part =
                name + QLatin1Char(' ') + codes.join(QLatin1Char(' '));
            if (list.reported > held) {
                part +=
                    QStringLiteral(" and %1 more").arg(list.reported - held);
            }
            parts << part;
            break;
        }
        case ECUIO_DIAG_REFUSED:
            parts << name + refused(kServices[l], list.nrc);
            break;
        case ECUIO_DIAG_UNANSWERED:
            parts << name + QStringLiteral(" unanswered");
            break;
        default:
            break;
        }
    }

    switch (s.readiness) {
    case ECUIO_DIAG_ANSWERED: {
        QStringList incomplete;
        const uint8_t n =
            std::min<uint8_t>(s.monitor_count, ECUIO_DIAG_MAX_MONITORS);
        for (uint8_t i = 0; i < n; ++i) {
            if ((s.monitors[i].flags & ECUIO_MONITOR_COMPLETE) == 0) {
                incomplete << QString::fromLatin1(
                    ecuio_monitor_short_name(s.monitors[i].id, s.flags));
            }
        }
        QString part = QStringLiteral("readiness %1 of %2 complete")
                           .arg(n - incomplete.size())
                           .arg(n);
        if (!incomplete.isEmpty()) {
            part += QStringLiteral(" (%1 incomplete)")
                        .arg(incomplete.join(QStringLiteral(", ")));
        }
        parts << part;
        break;
    }
    case ECUIO_DIAG_REFUSED:
        parts << QStringLiteral("readiness") + refused(0x01, s.readiness_nrc);
        break;
    case ECUIO_DIAG_UNANSWERED:
        parts << QStringLiteral("readiness unanswered");
        break;
    default:
        break;
    }

    switch (s.freeze) {
    case ECUIO_DIAG_ANSWERED:
        parts << ((s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0
                      ? QStringLiteral("freeze frame for %1")
                            .arg(dtc(s.freeze_dtc))
                      : QStringLiteral("no freeze frame"));
        break;
    case ECUIO_DIAG_REFUSED:
        parts << QStringLiteral("freeze frame") + refused(0x02, s.freeze_nrc);
        break;
    case ECUIO_DIAG_UNANSWERED:
        parts << QStringLiteral("freeze frame unanswered");
        break;
    default:
        break;
    }
    if (parts.isEmpty()) {
        parts << QStringLiteral("nothing asked");
    }
    return QStringLiteral("ECU %1 diagnostics: ").arg(hexId(s.rx_id)) +
           parts.join(QStringLiteral("; "));
}
