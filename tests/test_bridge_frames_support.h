// File/layer: tests/test_bridge_frames_support.h | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: the helpers of test_bridge_frames*.cpp (test_bridge)

#pragma once

#include <gtest/gtest.h>

#include <QFile>
#include <QHash>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <cstdio>
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
#include "bridge_support.h"
