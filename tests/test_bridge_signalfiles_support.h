// File/layer: tests/test_bridge_signalfiles_support.h | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: the helpers of test_bridge_signalfiles*.cpp (test_bridge)

#pragma once

#include <cmath>
#include <gtest/gtest.h>
#include <functional>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <vector>

#include "healthreport.h"
#include "packed_plugin.h"
#include "pluginstore.h"
#include "sessionbridge.h"   // SessionBridge + FrameListModel
#include "urlwrite.h"
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
