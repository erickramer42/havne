#ifndef ECUIO_TESTS_SESSION_FLASH_SUPPORT_H
#define ECUIO_TESTS_SESSION_FLASH_SUPPORT_H

#include "plugin_flash_support.h"
#include "session_operation_support.h"

#include "../core/src/operation_journal.h"

#include <cstdint>
#include <string>

namespace session_flash {

using namespace session_operation;
using flash_test::Flash;

constexpr uint32_t kCode = ECUIO_SIM_MEMORY_CODE_ADDRESS;
constexpr uint32_t kSector = ECUIO_SIM_FLASH_SECTOR_BYTES;

/* Two sectors of the sim's code region; the first holds what the module
 * holds already, the second a made-up image */
inline Bytes image_two_sectors()
{
    Bytes b(2 * kSector);
    for (uint32_t i = 0; i < kSector; ++i) {
        b[i] = ecuio_sim_memory_fill(kCode + i);
        b[kSector + i] = static_cast<uint8_t>(i * 37 + 3);
    }
    return b;
}

inline Flash sim_flash()
{
    Flash f;
    f.images[0].blob = image_two_sectors();
    f.sectors = R"([{"address": "00080000", "bytes": 4096}, )"
                R"({"address": "00081000", "bytes": 4096}])";
    return f;
}

inline conversation_test::Made flash_made(const Flash& f = sim_flash())
{
    conversation_test::Made m = made();
    m.operations = f.json();
    m.files_extra = R"(, "code.bin": {"kind": "bin", "base64": ")" +
                    conversation_test::base64(f.images[0].blob) + "\"}";
    return m;
}

inline void arm(Bench& b, const ecuio_sim_flash_faults& faults = {})
{
    ASSERT_TRUE(ecuio_sim_flash_arm(b.node.s.flash, b.node.s.memory, faults));
}

constexpr uint32_t kFlags =
    ECUIO_OPERATION_MAY_UNLOCK | ECUIO_OPERATION_BATTERY_SUPPORT;

inline ecuio_status_t start(Bench& b, const char* copy = nullptr)
{
    const ecuio_operation_request_t q = request(kFlags);
    return ecuio_session_operation_start_from(b.s, &q, b.folder.str().c_str(),
                                              copy);
}

inline size_t count_routine(const Bench& b, uint16_t id)
{
    size_t n = 0;
    for (const Bytes& q : b.sent) {
        n += q.size() >= 4 && q[0] == 0x31 && q[1] == 0x01 &&
                     q[2] == static_cast<uint8_t>(id >> 8) &&
                     q[3] == static_cast<uint8_t>(id)
                 ? 1u
                 : 0u;
    }
    return n;
}

inline ecuio_operation::journal_found journal_of(const Bench& b)
{
    std::string why;
    return ecuio_operation::journal_find(b.folder.str(), 0x7E8, &why);
}

} // namespace session_flash

#endif
