// File/layer: tests/cli_unlock_fixture.cpp | test-only, Qt-free
// Testing: HARNESS: CliConsentRunTest.AnUnlockOpensTheGatedLevelOnce
//          (tests/cli_consent.py unlock) runs it first
//
// Writes the two packed plugins the unlock run reads, built from the
// guest test_core's conversation cases run (conversation_session_support.h),
// so no committed bytes go stale: <right> computes the simulated ECU's key
// for level 01 on 7E0/7E8, <wrong> the same key off by one bit, with a
// minute's delay after a refused key, so a second run falls inside it.
//
//   cli_unlock_fixture <right.json> <wrong.json>

#include "conversation_session_support.h"

#include <cstdio>
#include <string>

namespace {

bool write_text(const char* path, const std::string& text)
{
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: cli_unlock_fixture <right.json> "
                             "<wrong.json>\n");
        return 2;
    }
    conversation_test::Made m = conversation_session::made3();
    const std::string right = m.json();
    conversation_session::GuestShape wrong;
    wrong.flip = 1;
    m.module = conversation_session::unlocking_guest(wrong).build();
    m.group.security =
        R"([{"ecu": "pcm", "level": "01", "seed_bytes": 4, "key_bytes": 4, )"
        R"("delay_ms": 60000, "key_window_ms": 1000}])";
    if (!write_text(argv[1], right) || !write_text(argv[2], m.json())) {
        std::fprintf(stderr, "cli_unlock_fixture: a file didn't write\n");
        return 1;
    }
    return 0;
}
