// File/layer: app/bridge_diagnostics_entries.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "bridgetext.h"

#include <QByteArray>
#include <QDate>
#include <QStringList>

#include <algorithm>
#include <cstring>

using namespace bridgetext;

namespace {

/* A clear's outcome (ADR-0038) as QML's key, and its NRC as "7F xx yy"
 * where one was answered */
QString clearOutcome(const ecuio_channel_diagnostics_t& s, QString* nrc)
{
    QString clear;
    switch (s.clear) {
    case ECUIO_CLEAR_CLEARED:
        clear = QStringLiteral("cleared");
        break;
    case ECUIO_CLEAR_REFUSED:
        clear = QStringLiteral("refused");
        *nrc = QStringLiteral("7F 04 %1").arg(hexByte(s.clear_nrc));
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
            *nrc = QStringLiteral("7F 01 %1").arg(hexByte(s.clear_nrc));
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
            *nrc = QStringLiteral("7F 01 %1").arg(hexByte(s.clear_nrc));
        }
        break;
    case ECUIO_CLEAR_NOT_EMISSIONS:
        clear = QStringLiteral("notEmissions");
        break;
    case ECUIO_CLEAR_NOT_OPENED:
        clear = QStringLiteral("notOpened");
        break;
    default:
        clear = QStringLiteral("none");
        break;
    }
    return clear;
}

} // namespace

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
BridgeFormats::inspectionEntry(const ecuio_channel_diagnostics_t* reports,
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

QVariantMap
BridgeFormats::diagnosticsEntry(const ecuio_channel_diagnostics_t& s,
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
    QString clearNrc;
    const QString clear = clearOutcome(s, &clearNrc);
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
