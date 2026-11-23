// File/layer: tests/test_j2534_car.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534CarTest (test_j2534_health); Cross-arch
//
// A car-sized session on the fake DLL (ADR-0065 section 10): 18 modules,
// each a 29-bit ECU of the fake's module table, read through the
// surrogate with the fake's filter gating on, so a module whose filter
// the fake didn't keep never answers. The table holds more rows than a
// page knob carries, so it reaches the child through the environment.

#include <gtest/gtest.h>

#include "ecuio/core.h"

#ifdef _WIN32
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "j2534_backend_fixture.h"

namespace {

constexpr int kModules = 18;
constexpr uint32_t kFirstEcu = 0x40; /* made up: ECUs 0x40 to 0x51 */
constexpr uint16_t kDid = 0x4A00;

uint32_t reply_id(int i)
{
    return 0x18DAF100u | (kFirstEcu + static_cast<uint32_t>(i));
}

/* Each module's answer: twelve bytes of text, longer than a frame */
std::string text_of(int i)
{
    char t[16];
    snprintf(t, sizeof t, "MODULE NO %02d", i + 1);
    return t;
}

/* The fake's table: "18DAF140:4A00=<hex>;..." */
std::string module_spec()
{
    std::string spec;
    char row[64];
    for (int i = 0; i < kModules; ++i) {
        snprintf(row, sizeof row, "%s%08X:%04X=", i == 0 ? "" : ";",
                 static_cast<unsigned>(reply_id(i)), kDid);
        spec += row;
        for (const char c : text_of(i)) {
            snprintf(row, sizeof row, "%02X", static_cast<unsigned char>(c));
            spec += row;
        }
    }
    return spec;
}

/* The session's did list: each module's DID as text */
std::string module_query()
{
    std::string q = "?profile=uds-generic&identify=0&diagnose=0&did=";
    char item[32];
    for (int i = 0; i < kModules; ++i) {
        snprintf(item, sizeof item, "%s%08X:%04X:text", i == 0 ? "" : ",",
                 static_cast<unsigned>(reply_id(i)), kDid);
        q += item;
    }
    return q;
}

/* The reply IDs whose answer was the module's own text */
struct Answered {
    std::mutex mutex;
    std::set<uint32_t> ids;
    std::set<uint32_t> wrong;

    static void on_frame(const ecuio_frame_t* f, void* user)
    {
        auto* self = static_cast<Answered*>(user);
        const uint32_t ecu = f->arbitration_id & 0xFFu;
        const int i = static_cast<int>(ecu) - static_cast<int>(kFirstEcu);
        const std::lock_guard<std::mutex> lock(self->mutex);
        if (i < 0 || i >= kModules || f->arbitration_id != reply_id(i)) {
            self->wrong.insert(f->arbitration_id);
            return;
        }
        const std::string want = text_of(i);
        const bool same = f->data_length == 3 + want.size() &&
                          f->data[0] == 0x62 && f->data[1] == 0x4A &&
                          f->data[2] == 0x00 &&
                          memcmp(f->data + 3, want.data(), want.size()) == 0;
        (same ? self->ids : self->wrong).insert(f->arbitration_id);
    }

    size_t count()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return ids.size();
    }
};

class J2534CarTest : public j2534_backends::BackendTest
{
protected:
    Answered answered; /* outlives the session, which TearDown closes */
    ecuio_session_t* session = nullptr;

    void TearDown() override
    {
        if (session != nullptr) {
            EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
            ecuio_session_destroy(session);
            session = nullptr;
        }
        BackendTest::TearDown();
    }
};

} // namespace

/* 18 modules on one channel's filters, with the fake dropping whatever
 * no filter it kept matches: live data holds J2534's budget of them open
 * (ADR-0065 step 5), the first in channel order, each answering its own
 * DID; the rest wait with no room and are asked nothing */
TEST_F(J2534CarTest, EighteenModulesEachAnswerThroughTheSurrogate)
{
    const std::string spec = module_spec();
    j2534_backends::EnvSwap table("ECUIO_FAKE_MODULE_DIDS", spec.c_str());
    ASSERT_NO_FATAL_FAILURE(knob("ECUIO_FAKE_FILTER_GATING", "1"));

    const std::string u = uri(module_query().c_str());
    ASSERT_EQ(ecuio_session_create(u.c_str(), &session), ECUIO_OK)
        << u << ": " << ecuio_last_error_detail();
    ASSERT_NO_FATAL_FAILURE(expect_opened_in_the_surrogate());
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.module_count, static_cast<uint32_t>(kModules));
    const int open = static_cast<int>(info.module_budget);
    ASSERT_EQ(open, 9) << "J2534's 10 filters less the one kept back";

    ASSERT_EQ(
        ecuio_register_frame_callback(session, Answered::on_frame, &answered),
        ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (answered.count() < static_cast<size_t>(open) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (int i = open; i < kModules; ++i) {
        ecuio_channel_support_t sup{};
        ASSERT_EQ(ecuio_session_get_channel_support(
                      session, static_cast<uint32_t>(i), &sup),
                  ECUIO_OK);
        EXPECT_NE(sup.flags & ECUIO_SUPPORT_NO_ROOM, 0u) << i;
    }
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    session = nullptr;

    const std::lock_guard<std::mutex> lock(answered.mutex);
    std::string missing;
    std::string asked;
    char hex[16];
    for (int i = open; i < kModules; ++i) {
        if (answered.ids.count(reply_id(i)) != 0) {
            snprintf(hex, sizeof hex, " %08X",
                     static_cast<unsigned>(reply_id(i)));
            asked += hex;
        }
    }
    EXPECT_TRUE(asked.empty()) << "a module with no room answered:" << asked;
    for (int i = 0; i < open; ++i) {
        if (answered.ids.count(reply_id(i)) == 0) {
            snprintf(hex, sizeof hex, " %08X",
                     static_cast<unsigned>(reply_id(i)));
            missing += hex;
        }
    }
    EXPECT_TRUE(missing.empty()) << "no answer of its own from" << missing;
    EXPECT_TRUE(answered.wrong.empty())
        << answered.wrong.size() << " replies on an ID or with bytes not "
        << "a module's own";
}

#endif /* _WIN32 */
