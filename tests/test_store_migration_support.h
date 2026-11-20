// File/layer: tests/test_store_migration_support.h | GTest + QtCore, app-layer tests
// Testing: BRIDGE: the helpers of test_store_migration*.cpp (test_bridge)

#pragma once

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <iostream>
#include <utility>

#include "obdbimport.h"
#include "packed_plugin.h"
#include "pluginstore.h"
#include "sessionbridge.h"
#include "storemigration.h"
#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include "old_state.h"

using namespace old_state;
