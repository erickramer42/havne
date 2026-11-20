// File/layer: app/bridgetext.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#pragma once

#include <QString>
#include <QDir>
#include <QStandardPaths>
#include <QtGlobal>

#include "ecuio/core.h"

// What the bridge's source files say in more than one of them: a path for
// display, a failed call in words, the activity log's numbers. Internal:
// only the bridge's own .cpp files include it.
namespace bridgetext {

// A file or folder for display: relative to Documents when under it,
// so the account name stays out of the log and the page
inline QString documentsRelative(const QString& path)
{
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!docs.isEmpty()) {
        const QString rel = QDir(docs).relativeFilePath(path);
        if (QDir::isRelativePath(rel) && !rel.startsWith(QLatin1String(".."))) {
            return QDir::toNativeSeparators(QStringLiteral("Documents/") + rel);
        }
    }
    return QDir::toNativeSeparators(path);
}

// A failed create or start, in words: the status, then core's detail
// (which URI key, which vendor call, the vendor's text) when it has one.
// Call it on the thread that made the call: the detail is thread-local.
inline QString failureText(ecuio_status_t rc)
{
    QString status = QString::fromUtf8(ecuio_status_string(rc));
    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || detail[0] == '\0') {
        return status;
    }
    return status + QStringLiteral(": ") + QString::fromUtf8(detail);
}

// The activity log's numbers: an ID or count as 0x7E8, a byte as 0C.
inline QString hexId(uint32_t v)
{
    return QStringLiteral("0x") + QString::number(v, 16).toUpper();
}

inline QString hexByte(uint8_t b)
{
    return QStringLiteral("%1").arg(static_cast<uint>(b), 2, 16,
                                    QLatin1Char('0')).toUpper();
}

// "1 signal", "2 signals": the activity log's count of a file's signals
inline QString signalCountText(int count)
{
    return count == 1 ? QStringLiteral("1 signal")
                      : QStringLiteral("%1 signals").arg(count);
}

// Core's words for a refused call, else its status
inline QString detailOr(ecuio_status_t rc)
{
    const char* detail = ecuio_last_error_detail();
    return detail != nullptr && detail[0] != '\0'
               ? QString::fromUtf8(detail)
               : QString::fromUtf8(ecuio_status_string(rc));
}

// A clear's vehicle speed in the unit system, converted by core
// (ADR-0022): "48 km/h", "30 mph"; "" if core can't convert it.
inline QString clearSpeedText(uint32_t kmh, uint32_t units)
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

} // namespace bridgetext
