// File/layer: tests/test_plugin_conversation_gate.cpp | C++17, Qt-free,
//             every platform
// Testing: UNIT: PluginConversationTest (test_signals)
//
// The release gate for the conversation tier: a plugin that carries the
// tier loads as data and, in a session, starts no module (ADR-0066, tier 2
// ships inert). The sandbox's counters show it: no instance was made, so
// the runtime allocated nothing and holds nothing.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/profile.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "plugin_conversation_support.h"
#include "wasm_sandbox.h" /* core/src: runtime_allocations */

namespace {

using conversation_test::conversation_of;
using conversation_test::load_packed;
using conversation_test::Made;

std::atomic<int> g_frames{0};

void count_frame(const ecuio_frame_t*, void*)
{
    g_frames.fetch_add(1);
}

TEST(PluginConversationTest,
     APluginWithTheTierInASessionIsDataAloneAndStartsNoRuntime)
{
    const uint64_t allocations = ecuio_wasm::runtime_allocations();
    const size_t bytes = ecuio_wasm::runtime_bytes_in_use();

    Made m;
    const std::string json = m.json();
    {
        auto p = load_packed(json);
        ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
        char name[ECUIO_PROFILE_NAME_MAX + 1] = {};
        ASSERT_EQ(ecuio_plugin_register(p.get(), 0, name, sizeof name),
                  ECUIO_OK)
            << ecuio_last_error_detail();

        /* The profile goes before the plugin does (declared after it) */
        struct Unregister {
            const char* name;
            ~Unregister() { EXPECT_EQ(ecuio_profile_unregister(name), ECUIO_OK); }
        } unregister{name};
        /* A plugin with the tier: its group reads back */
        EXPECT_GT(conversation_of(p.get()).budget, 0u)
            << "the plugin carries the conversation tier";

        const std::string uri = std::string("mock://x?profile=") + name;
        ecuio_session_t* s = nullptr;
        ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK)
            << ecuio_last_error_detail();
        g_frames = 0;
        ASSERT_EQ(ecuio_register_frame_callback(s, &count_frame, nullptr),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(s), ECUIO_OK)
            << ecuio_last_error_detail();
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (g_frames.load() == 0 && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_GT(g_frames.load(), 0) << "the session ran and delivered";
        ASSERT_EQ(ecuio_session_close(s), ECUIO_OK);
        ASSERT_EQ(ecuio_session_destroy(s), ECUIO_OK);
    }

    EXPECT_EQ(ecuio_wasm::runtime_allocations(), allocations)
        << "a module was instantiated";
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use(), bytes);
}

} // namespace
