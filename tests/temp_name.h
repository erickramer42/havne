// File/layer: tests/temp_name.h | test-only, C++17, Qt-free
// Testing: TempNameTest (test_core)
//
// A scratch file or folder's name of this process's own:
// <prefix>-<pid>-<the running test's name>, or <prefix>-<pid> outside a
// test. Every checkout on a machine shares the temporary directory, so a
// name keyed only by the test lets two runs of one case, from two
// checkouts at once, clear or overwrite each other's files. The
// benchmarks (bench_recording, bench_timing) keep fixed names: they run
// by hand and alone.

#ifndef ECUIO_TESTS_TEMP_NAME_H
#define ECUIO_TESTS_TEMP_NAME_H

#include <gtest/gtest.h>

#include <string>

#ifdef _WIN32
#include <process.h> /* _getpid */
#else
#include <unistd.h> /* getpid */
#endif

inline std::string temp_name(const std::string& prefix)
{
#ifdef _WIN32
    const long pid = _getpid();
#else
    const long pid = static_cast<long>(getpid());
#endif
    std::string name = prefix + "-" + std::to_string(pid);
    const ::testing::TestInfo* test =
        ::testing::UnitTest::GetInstance()->current_test_info();
    if (test != nullptr) {
        name += std::string("-") + test->name();
    }
    return name;
}

#endif
