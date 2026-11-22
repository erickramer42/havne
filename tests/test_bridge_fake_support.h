// File/layer: tests/test_bridge_fake_support.h | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: the helpers of test_bridge_fake*.cpp (test_bridge)

#pragma once

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <vector>

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
#include "car_sized_set.h"
