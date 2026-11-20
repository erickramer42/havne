// File/layer: tests/test_bridge_replay_support.h | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: the helpers of test_bridge_replay*.cpp (test_bridge)

#pragma once

#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <initializer_list>
#include <thread>

#include "carryover.h"
#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include <ecuio/version.h>
#include <ecuio/serial.h>
#include <ecuio/signal_sets.h>
#include "ecuio/internal.h" // core's recorder seams (a failed write)
#include <ecuio/obdb.h> // ECUIO_OBDB_MAX_BYTES

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "fake_control.h"   // the fake J2534 DLL's control page
#endif
#include "bridge_support.h"

namespace {

/* The replay URI that decodes a recording as it was decoded live: the
 * recorded URI's profile and DIDs, or its scheme's default profile */
inline QString replayUriFor(const QString& folder)
{
    QString uri;
    for (const QByteArray& line : readFile(folder + QStringLiteral("/events.log")).split('\n')) {
        if (line.startsWith("uri ")) {
            uri = QString::fromUtf8(line.mid(4).trimmed());
            break;
        }
    }
    if (uri.isEmpty())
        return {};
    QString profile = uri.startsWith(QStringLiteral("mock://")) ? QStringLiteral("mock")
                                                                : QStringLiteral("obd2");
    QStringList keep;
    const qsizetype q = uri.indexOf(QLatin1Char('?'));
    if (q >= 0) {
        for (const QString& pair : uri.mid(q + 1).split(QLatin1Char('&'))) {
            if (pair.startsWith(QStringLiteral("profile=")))
                profile = pair.mid(8);
            else if (pair.startsWith(QStringLiteral("did=")))
                keep << pair;
        }
    }
    keep.prepend(QStringLiteral("profile=") + profile);
    return QStringLiteral("replay://%1?%2&speed=max").arg(folder, keep.join(QLatin1Char('&')));
}

} // namespace
