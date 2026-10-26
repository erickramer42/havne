// File/layer: tests/j2534_backend_fixture.h | GTest, HARNESS tier
// Testing: the base of J2534HealthTest (test_j2534_health) and
//          J2534ConnectTest (test_j2534_connect)
//
// Every session runs in a surrogate (ADR-0025): in this tree the x64
// fake in the x64 surrogate beside the test; with ECUIO_FAKE_DLL set
// (the cross-arch run), the Win32 tree's fake in the x86 surrogate
// that ECUIO_SURROGATE_EXE names. Knobs go through the fake's control
// page (fake_control.h), the one channel that reaches the child's
// instance mid-session; the page also shows which process opened the
// fake, so each case checks it ran in the surrogate.

#ifndef ECUIO_TESTS_J2534_BACKEND_FIXTURE_H
#define ECUIO_TESTS_J2534_BACKEND_FIXTURE_H

#include <gtest/gtest.h>

#include "ecuio/core.h"
#include "ecuio/internal.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "fake_control.h"

namespace j2534_backends {

/* Replaces a variable for one scope and puts the previous value back:
 * the cross-arch run sets ECUIO_SURROGATE_EXE itself. */
struct EnvSwap {
    const char* name;
    bool had = false;
    char old[1024] = {};
    EnvSwap(const char* n, const char* v) : name(n)
    {
        const DWORD k = GetEnvironmentVariableA(n, old, sizeof(old));
        had = k > 0 && k < sizeof(old);
        SetEnvironmentVariableA(n, v);
    }
    ~EnvSwap() { SetEnvironmentVariableA(name, had ? old : nullptr); }
    EnvSwap(const EnvSwap&) = delete;
    EnvSwap& operator=(const EnvSwap&) = delete;
};

class BackendTest : public ::testing::Test {
protected:
    fake_control::Page* page = nullptr;
    std::string dll;

    void SetUp() override
    {
        page = fake_control::create_for_this_process();
        ASSERT_NE(page, nullptr);
        fake_control::reset(page);

        const char* cross = std::getenv("ECUIO_FAKE_DLL");
        dll = (cross != nullptr && cross[0] != '\0') ? cross
                                                     : FAKE_J2534_DLL_PATH;
    }

    /* A derived TearDown closes its sessions first, then calls this. */
    void TearDown() override
    {
        if (page != nullptr) {
            fake_control::reset(page);   /* no knob reaches the next case */
        }
    }

    void knob(const char* name, const char* value)
    {
        ASSERT_TRUE(fake_control::set(page, name, value)) << name;
    }

    std::string uri(const char* query) const
    {
        return "j2534://" + dll + query;
    }

    /* The fake publishes the process that opened it: a surrogate's,
     * never this one. */
    void expect_opened_in_the_surrogate() const
    {
        const unsigned long opener = fake_control::opener_pid(page);
        ASSERT_NE(opener, 0ul) << "the fake never opened";
        ASSERT_NE(opener, GetCurrentProcessId())
            << "the fake opened in this process, not in the surrogate";
    }
};

} // namespace j2534_backends

#endif /* ECUIO_TESTS_J2534_BACKEND_FIXTURE_H */
