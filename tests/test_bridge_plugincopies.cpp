// File/layer: tests/test_bridge_plugincopies.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest.PluginCopyThatNoLongerLoadsSaysSo (test_bridge)
//
// A plugin's kept copy read again at the store's first list
// (PluginStore::all): one core no longer loads is refused in its words.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QVariantList>
#include <QVariantMap>

#include <ecuio/plugin.h>

#include "packed_plugin.h"
#include "pluginstore.h"
#include "sessionbridge.h"
#include "bridge_support.h"

/* A copy named by its own hash whose bytes core no longer loads (a format
 * past this Havne's) is refused at the first list in the loader's words,
 * registers nothing, and Remove deletes it */
TEST(BridgeTest, PluginCopyThatNoLongerLoadsSaysSo)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QByteArray bytes = packed_plugin::example();
    ASSERT_TRUE(bytes.contains("\"havne_plugin\": 1,"));
    const QByteArray past = QByteArray::number(ECUIO_PLUGIN_FORMAT_MAX + 1);
    bytes.replace("\"havne_plugin\": 1,", "\"havne_plugin\": " + past + ",");
    const QString sha = sha256Of(bytes);
    ASSERT_TRUE(QDir().mkpath(PluginStore::root()));
    QFile copy(PluginStore::root() + QLatin1Char('/') + sha +
               QStringLiteral(".havne-plugin.json"));
    ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
    ASSERT_EQ(copy.write(bytes), bytes.size());
    copy.close();

    SessionBridge bridge;
    const QVariantList all = bridge.plugins();
    ASSERT_EQ(all.size(), 1);
    const QVariantMap e = all.at(0).toMap();
    EXPECT_EQ(e.value(QStringLiteral("sha256")).toString(), sha);
    EXPECT_FALSE(e.value(QStringLiteral("ok")).toBool());
    const QString words = e.value(QStringLiteral("words")).toString();
    EXPECT_TRUE(words.startsWith(QStringLiteral("It no longer loads: ")))
        << words.toStdString();
    EXPECT_TRUE(words.contains(QStringLiteral("format ") +
                               QString::number(ECUIO_PLUGIN_FORMAT_MAX + 1)))
        << words.toStdString();
    EXPECT_FALSE(registered("plugin.example-sim.base"));
    EXPECT_TRUE(bridge.removePlugin(sha).value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(copy.exists());
    EXPECT_TRUE(bridge.plugins().isEmpty());
}
