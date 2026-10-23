// File/layer: tests/j2534_backend_fixture.h | GTest, HARNESS tier
// Testing: the base of the suites that run each case once per backend:
//          J2534HealthTest (test_j2534_health) and J2534ConnectTest
//          (test_j2534_connect)
//
// /InProc loads the x64 fake in this process. /Surrogate sends it
// through the surrogate: in this tree the x64 surrogate, with the
// loader's routing forced (ecuio_j2534_force_surrogate_for_testing);
// with ECUIO_FAKE_DLL set (the cross-arch run), the Win32 tree's fake
// in the x86 surrogate that ECUIO_SURROGATE_EXE names. Knobs go
// through the fake's control page (fake_control.h), the one channel
// that reaches the child's instance mid-session; the page also shows
// which process opened the fake, so each case checks its backend.

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

enum class Backend { InProc, Surrogate };

inline std::string label(const ::testing::TestParamInfo<Backend>& info)
{
    return info.param == Backend::InProc ? "InProc" : "Surrogate";
}

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

class BackendTest : public ::testing::TestWithParam<Backend> {
protected:
    fake_control::Page* page = nullptr;
    std::string dll;
    std::unique_ptr<EnvSwap> exe_swap;

    void SetUp() override
    {
        page = fake_control::create_for_this_process();
        ASSERT_NE(page, nullptr);
        fake_control::reset(page);

        if (GetParam() == Backend::InProc) {
            dll = FAKE_J2534_DLL_PATH;
            return;
        }
        const char* cross = std::getenv("ECUIO_FAKE_DLL");
        if (cross != nullptr && cross[0] != '\0') {
            dll = cross;   /* x86: its surrogate is already named */
        } else {
            dll = FAKE_J2534_DLL_PATH;
            exe_swap = std::make_unique<EnvSwap>("ECUIO_SURROGATE_EXE",
                                                 SURROGATE_EXE_PATH);
        }
        ecuio_j2534_force_surrogate_for_testing(true);
    }

    /* A derived TearDown closes its sessions first, then calls this. */
    void TearDown() override
    {
        ecuio_j2534_force_surrogate_for_testing(false);
        exe_swap.reset();
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

    /* The fake publishes the process that opened it: this one in-proc,
     * another through the surrogate. */
    void expect_opened_on_backend() const
    {
        const unsigned long opener = fake_control::opener_pid(page);
        if (GetParam() == Backend::InProc) {
            ASSERT_EQ(opener, GetCurrentProcessId())
                << "the fake didn't open in this process";
        } else {
            ASSERT_NE(opener, 0ul) << "the fake never opened";
            ASSERT_NE(opener, GetCurrentProcessId())
                << "the fake opened in-proc, not in the surrogate";
        }
    }
};

} // namespace j2534_backends

#endif /* ECUIO_TESTS_J2534_BACKEND_FIXTURE_H */
