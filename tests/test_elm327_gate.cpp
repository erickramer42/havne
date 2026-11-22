// File/layer: tests/test_elm327_gate.cpp | GTest, HARNESS tier, headless
//             CI job (every platform)
// Testing: HARNESS: Elm327SessionTest (test_elm327)
//
// The conversation tier's release gate on a requesting transport: a
// plugin with the tier in a session on the fake ELM327 starts no module
// (ADR-0066, tier 2 ships inert), as on mock:// (test_plugin_conversation
// _gate.cpp). A requesting transport's attach is where an instance would
// be made, so the gate is judged there too.

#include "test_elm327_support.h"

#include "plugin_conversation_support.h"
#include "wasm_sandbox.h" /* core/src: runtime_allocations */

#include <ecuio/plugin.h>

TEST_F(Elm327SessionTest, APluginWithTheTierStartsNoRuntimeOnTheAdapter)
{
    const uint64_t allocations = ecuio_wasm::runtime_allocations();
    const size_t bytes = ecuio_wasm::runtime_bytes_in_use();

    const conversation_test::Made m;
    auto p = conversation_test::load_packed(m.json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    char name[ECUIO_PROFILE_NAME_MAX + 1] = {};
    ASSERT_EQ(ecuio_plugin_register(p.get(), 0, name, sizeof name), ECUIO_OK)
        << ecuio_last_error_detail();
    /* The profile goes before the plugin does (declared after it) */
    struct Unregister {
        const char* name;
        ~Unregister() { EXPECT_EQ(ecuio_profile_unregister(name), ECUIO_OK); }
    } unregister{name};
    EXPECT_GT(conversation_test::conversation_of(p.get()).budget, 0u)
        << "the plugin carries the conversation tier";
    {
        Session s;
        ASSERT_EQ(s.create(uri(std::string("?profile=") + name)), ECUIO_OK)
            << ecuio_last_error_detail();
        s.start();
        EXPECT_TRUE(wait_for(
            [&] {
                std::lock_guard<std::mutex> g(s.cap.m);
                return !s.cap.frames.empty();
            },
            5000))
            << "the session asked and was answered";
        ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    }
    EXPECT_EQ(ecuio_wasm::runtime_allocations(), allocations)
        << "a module was instantiated";
    EXPECT_EQ(ecuio_wasm::runtime_bytes_in_use(), bytes);
}
