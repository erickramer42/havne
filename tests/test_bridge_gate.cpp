// File/layer: tests/test_bridge_gate.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest.AConversationPluginImportedAndConnectedStartsNoRuntime
//          (test_bridge)
//
// The release gate for the conversation tier on the app's side: a plugin
// that carries the tier imports through the store and decodes a session as
// data alone (ADR-0066, tier 2 ships inert). core's sandbox counters show
// no instance was made. The core half is PluginConversationTest.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

#include "plugin_conversation_support.h"
#include "sessionbridge.h"
#include "bridge_support.h"
#include "wasm_sandbox.h" /* core/src: runtime_allocations */

TEST(BridgeTest, AConversationPluginImportedAndConnectedStartsNoRuntime)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const uint64_t allocations = ecuio_wasm::runtime_allocations();
    const size_t bytes = ecuio_wasm::runtime_bytes_in_use();
    const QByteArray text =
        QByteArray::fromStdString(conversation_test::Made{}.json());
    {
        /* The store doesn't report a plugin's tiers: core reads the same
         * bytes, so the package does carry the group */
        const auto loaded = conversation_test::load_packed(text.toStdString());
        ASSERT_NE(loaded, nullptr) << ecuio_last_error_detail();
        EXPECT_GT(conversation_test::conversation_of(loaded.get()).budget, 0u);
    }
    {
        SessionBridge bridge;
        const QVariantMap r = bridge.importPlugin(
            writeScratch(from.path(), QStringLiteral("t.json"), text));
        ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
            << r.value(QStringLiteral("words")).toString().toStdString();
        ASSERT_TRUE(bridge
                        .connectSession(QStringLiteral(
                            "mock://gate?profile=plugin.talker.base"))
                        .isEmpty());
        EXPECT_TRUE(spinUntil([&bridge]() { return bridge.frameCount() > 0; },
                              5000))
            << "the session delivered";
        EXPECT_TRUE(disconnectAndWait(bridge));
        EXPECT_EQ(removeEveryPlugin(bridge), 1);
    }
    EXPECT_EQ(ecuio_wasm::runtime_allocations(), allocations)
        << "a module was instantiated";
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use(), bytes);
}
