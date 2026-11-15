// File/layer: app/bridge_sessioninfo.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "bridgetext.h"

#include <QByteArray>
#include <QRandomGenerator>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>

using namespace bridgetext;

QString SessionBridge::newSalt() const
{
    QByteArray bytes(16, '\0');
    for (char& b : bytes) {
        b = static_cast<char>(QRandomGenerator::system()->bounded(256));
    }
    return QString::fromLatin1(bytes.toHex());
}

void SessionBridge::setVehicleLabel(const QString& label)
{
    if (label == m_vehicleLabel) {
        return;
    }
    m_vehicleLabel = label;
    emit vehicleLabelChanged();
}

QString SessionBridge::folderLabel(const QString& label)
{
    QString out;
    for (const QChar ch : label) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char(' ') ||
            ch == QLatin1Char('-') || ch == QLatin1Char('_') ||
            ch == QLatin1Char('.')) {
            out += ch;
        }
    }
    out = out.simplified().left(40);
    // Windows refuses a folder name ending in a dot or a space
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) {
        out.chop(1);
    }
    return out;
}

/* The URI as the header pill and the Dashboard show it. A direct-load
 * selector (one holding a separator, as resolve_uri tells a path from a
 * vendor name) is cut to the DLL's file name, j2534://.../vendor.dll, so
 * a local folder never reaches the screen or a screenshot, and a long
 * path can't elide the DLL off the pill's end. The query stays; every
 * other URI is unchanged. Settings and the activity log keep it whole. */
QString SessionBridge::uriLabelFor(const QString& uri)
{
    // A Bluetooth adapter's stream (ADR-0028): the address is personal
    // data, and the label reaches the lock screen through the session's
    // notification, so only its last two octets stay, enough to tell two
    // adapters apart
    const QString elm = QStringLiteral("elm327://");
    if (uri.startsWith(elm)) {
        const qsizetype query = uri.indexOf(QLatin1Char('?'), elm.size());
        const qsizetype end = query < 0 ? uri.size() : query;
        const QString port = uri.mid(elm.size(), end - elm.size());
        const qsizetype colon = port.indexOf(QLatin1Char(':'));
        const QString kind = port.left(colon);
        const qsizetype kept = 6; // ":AB:CD"
        if (colon > 0 &&
            (kind == QLatin1String("bt") || kind == QLatin1String("ble")) &&
            port.size() - colon - 1 > kept) {
            return elm + kind + QLatin1Char(':') + QChar(0x2026) +
                   port.right(kept) + uri.mid(end);
        }
        return uri;
    }
    const bool replay = uri.startsWith(QStringLiteral("replay://"));
    const QString scheme =
        replay ? QStringLiteral("replay://") : QStringLiteral("j2534://");
    if (!uri.startsWith(scheme)) {
        return uri;
    }
    const qsizetype query = uri.indexOf(QLatin1Char('?'), scheme.size());
    const qsizetype end = query < 0 ? uri.size() : query;
    QString selector = uri.mid(scheme.size(), end - scheme.size());
    const auto lastSep = [](const QString& s) {
        return std::max(s.lastIndexOf(QLatin1Char('/')),
                        s.lastIndexOf(QLatin1Char('\\')));
    };
    if (replay) {
        // The recording's folder name, and its frames.csv when named
        while (selector.size() > 1 &&
               lastSep(selector) == selector.size() - 1) {
            selector.chop(1);
        }
        qsizetype sep = lastSep(selector);
        if (sep > 0 &&
            selector.mid(sep + 1).compare(QStringLiteral("frames.csv"),
                                          Qt::CaseInsensitive) == 0) {
            sep = lastSep(selector.left(sep));
        }
        if (sep < 0) {
            return uri;
        }
        return scheme + QChar(0x2026) + QLatin1Char('/') +
               selector.mid(sep + 1) + uri.mid(end);
    }
    const qsizetype sep = lastSep(selector);
    if (sep < 0) {
        return uri;
    }
    return scheme + QChar(0x2026) + QLatin1Char('/') + selector.mid(sep + 1) +
           uri.mid(end);
}

/* What the session runs on, for the activity log (and so the
 * recording's events.log). Static, like linkStateForError, so
 * test_bridge pins the wording without an adapter. */
QString SessionBridge::sessionInfoText(const ecuio_session_info_t& info)
{
    const QString profile = QStringLiteral("profile ") + QString::fromUtf8(
        info.profile, static_cast<qsizetype>(
            qstrnlen(info.profile, ECUIO_SESSION_INFO_PROFILE_LEN)));
    if (info.backend == ECUIO_BACKEND_MOCK) {
        return QStringLiteral("Session: mock transport; ") + profile;
    }
    if (info.backend == ECUIO_BACKEND_REPLAY) {
        // The path is in the "Connected:" line just before
        const QString speed =
            info.replay_speed_milli == 0
                ? QStringLiteral("at full speed")
                : QStringLiteral("at %1x").arg(QString::number(
                      info.replay_speed_milli / 1000.0, 'g', 6));
        const bool loop = (info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP) != 0;
        const bool faults =
            (info.flags & ECUIO_SESSION_INFO_REPLAY_FAULTS) != 0;
        // A last row cut short by a crash or a power cut (ADR-0049 section 7)
        const QString torn =
            info.replay_rows_torn != 0
                ? QStringLiteral(", its last row cut short and left out")
                : QString();
        return QStringLiteral("Session: replay of a recording, %1 frames over "
                              "%2 s %3%4%5%6; ")
                   .arg(info.replay_frames)
                   .arg(QString::number(info.replay_span_ms / 1000.0, 'f', 1),
                        speed, loop ? QStringLiteral(", looping") : QString(),
                        faults ? QStringLiteral(", with faults injected")
                               : QString(),
                        torn) +
               profile;
    }
    if (info.backend == ECUIO_BACKEND_SOCKETCAN) {
        // The interface is in the "Connected:" line just before. A real
        // controller has a bit rate (core reads it from the interface);
        // vcan has none. A profile that requests runs through ISO-TP on
        // its channels; any other only listens
        const bool listenOnly =
            (info.flags & ECUIO_SESSION_INFO_LISTEN_ONLY) != 0;
        const QString bus =
            listenOnly ? QStringLiteral(
                             "a CAN controller in listen-only mode at %1 bit/s")
                             .arg(info.baud)
            : info.baud != 0
                ? QStringLiteral("a CAN controller at %1 bit/s").arg(info.baud)
                : QStringLiteral("a virtual CAN bus");
        const QString mode =
            info.channel_count > 0
                ? (info.channel_count == 1
                       ? QStringLiteral("requesting on 1 channel")
                       : QStringLiteral("requesting on %1 channels")
                             .arg(info.channel_count))
                : QStringLiteral("listening");
        return QStringLiteral("Session: SocketCAN, %1, %2; ").arg(bus, mode) +
               profile;
    }
    const bool elm327 = info.backend == ECUIO_BACKEND_ELM327;
    if (!elm327 && info.backend != ECUIO_BACKEND_J2534_SURROGATE) {
        return QStringLiteral("Session: backend %1").arg(info.backend);
    }
    const QString versions = versionsText(info);
    const QString versionsOrNone =
        versions.isEmpty() ? QStringLiteral("no version reading") : versions;

    QStringList parts;
    if (elm327) {
        // The port is in the "Connected:" line just before
        parts << QStringLiteral("ELM327 adapter (%1)").arg(versionsOrNone);
    } else {
        const QString arch =
            (info.dll_arch == ECUIO_DLL_ARCH_X86) ? QStringLiteral("x86 DLL")
            : (info.dll_arch == ECUIO_DLL_ARCH_X64)
                ? QStringLiteral("x64 DLL")
                : QStringLiteral("DLL of unknown architecture");
        parts << arch + QStringLiteral(", through the surrogate");
    }

    QString link = (info.protocol_id == 6) ? QStringLiteral("ISO 15765")
                 : (info.protocol_id == 5) ? QStringLiteral("CAN")
                 : QStringLiteral("protocol %1").arg(info.protocol_id);
    link += QStringLiteral(" at %1 baud").arg(info.baud);
    if (info.connect_flags != 0) {
        link += QStringLiteral(", flags ") + hexId(info.connect_flags);
    }
    parts << link;

    const uint32_t channels = std::min<uint32_t>(info.channel_count,
                                                 ECUIO_SESSION_INFO_MAX_CHANNELS);
    if (channels == 0) {
        parts << profile + QStringLiteral(", no channels (passive)");
    } else {
        QStringList pairs;
        for (uint32_t i = 0; i < channels; ++i) {
            pairs << hexId(info.rx_ids[i]) + QLatin1Char('/') + hexId(info.tx_ids[i]);
        }
        parts << QStringLiteral("%1, %2 %3 (rx/tx)")
                     .arg(profile,
                          channels == 1 ? QStringLiteral("channel")
                                        : QStringLiteral("channels"),
                          pairs.join(QStringLiteral(", ")));
    }

    parts << ((info.flags & ECUIO_SESSION_INFO_VBATT)
                  ? QStringLiteral("battery %1 V").arg(info.vbatt_mv / 1000.0, 0, 'f', 1)
                  : QStringLiteral("no battery reading"));
    if (elm327) {
        // An STN with parameter 2A on and its bit 2 clear stops listening
        // at a 7F xx 78, before the answer comes (ADR-0026)
        if ((info.flags & ECUIO_SESSION_INFO_PENDING_OFF) != 0) {
            parts << QStringLiteral(
                "the adapter is set not to wait past 7F xx 78 "
                "(STN parameter 2A), so a pending reply's "
                "answer is lost");
        }
        return QStringLiteral("Session: ") + parts.join(QStringLiteral("; "));
    }
    parts << versionsOrNone;
    return QStringLiteral("Session: ") + parts.join(QStringLiteral("; "));
}

/* The "Session:" line's versions, which Settings also keeps for each
 * device: an ELM327's AT I answer, then STI's on an STN chip; a J2534
 * adapter's firmware, DLL and API. "" without a reading, and for the
 * backends that have no adapter. */
QString SessionBridge::versionsText(const ecuio_session_info_t& info)
{
    if ((info.flags & ECUIO_SESSION_INFO_VERSIONS) == 0) {
        return {};
    }
    const auto text = [](const char* s) {
        const QString t = QString::fromUtf8(
            s,
            static_cast<qsizetype>(qstrnlen(s, ECUIO_SESSION_INFO_TEXT_LEN)));
        return t.isEmpty() ? QStringLiteral("(none)") : t;
    };
    if (info.backend == ECUIO_BACKEND_ELM327) {
        return text(info.firmware_version);
    }
    if (info.backend == ECUIO_BACKEND_J2534_SURROGATE) {
        return QStringLiteral("firmware %1, DLL %2, API %3")
            .arg(text(info.firmware_version), text(info.dll_version),
                 text(info.api_version));
    }
    return {};
}
