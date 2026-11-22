// File/layer: app/bridge_diagnostics_text.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "bridgetext.h"

#include <QStringList>

#include <algorithm>

using namespace bridgetext;

/* One channel's "ECU 0x… identity:" line: each answered item by its
 * name and text, then the refused requests with their codes, then how
 * many went unanswered. Static, like channelSupportText, so test_bridge
 * pins the wording without an adapter. */
QString BridgeFormats::channelIdentityText(const ecuio_channel_identity_t& s)
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
QString BridgeFormats::channelClearText(const ecuio_channel_diagnostics_t& s,
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
BridgeFormats::channelDiagnosticsText(const ecuio_channel_diagnostics_t& s)
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
