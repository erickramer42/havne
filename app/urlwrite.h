// File/layer: app/urlwrite.h | Qt6 Core, app layer, C++17, header-only
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// Bytes to the file a picker chose. A local file is written whole or not
// at all: QSaveFile writes beside it and renames, so a failed write leaves
// what was there. Android's document picker hands back a content:// URI,
// which QFile writes through its provider, where a rename can't happen.

#pragma once

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QSaveFile>
#include <QUrl>

inline bool writeToUrl(const QUrl& url, const QByteArray& bytes)
{
    if (url.isLocalFile()) {
        QSaveFile out(url.toLocalFile());
        return out.open(QIODevice::WriteOnly) &&
               out.write(bytes) == bytes.size() && out.commit();
    }
    QFile out(url.toString());
    return out.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           out.write(bytes) == bytes.size() && out.flush();
}
