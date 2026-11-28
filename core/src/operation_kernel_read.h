#ifndef ECUIO_OPERATION_KERNEL_READ_H
#define ECUIO_OPERATION_KERNEL_READ_H

#include "operation_plan.h"
#include "request_talk.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ecuio_operation {

constexpr uint32_t kWireMost = ECUIO_ENGINE_WRITE_MAX;

enum class helper_stage : uint8_t { open, block, exit, start };

enum class helper_took : uint8_t { next, started, bad };

struct helper_run {
    helper_stage at = helper_stage::open;
    bool running = false;
    uint8_t counter = 0;
    uint32_t block = 0;
    uint64_t sent = 0;
    uint32_t downloads = 0;
    uint32_t starts = 0;
    uint8_t status = 0;
    bool status_given = false;
};

inline uint8_t helper_service(const helper_run& h)
{
    switch (h.at) {
    case helper_stage::open:
        return 0x34;
    case helper_stage::block:
        return 0x36;
    case helper_stage::exit:
        return 0x37;
    default:
        return 0x31;
    }
}

inline uint32_t helper_block_bytes(const kernel& k, const helper_run& h)
{
    const uint64_t left = k.blob.size() - h.sent;
    return static_cast<uint32_t>(left < h.block ? left : h.block);
}

inline uint8_t* helper_put_be(uint8_t* out, uint64_t value, unsigned n)
{
    for (unsigned i = n < 4 ? n : 4; i > 0; --i) {
        *out++ = static_cast<uint8_t>(value >> (8 * (i - 1)));
    }
    return out;
}

/* out holds kWireMost bytes */
inline uint32_t helper_request(const kernel& k, const helper_run& h,
                               uint8_t* out)
{
    uint8_t* at = out;
    *at++ = helper_service(h);
    if (h.at == helper_stage::open) {
        *at++ = k.dfi;
        *at++ = static_cast<uint8_t>((k.size_bytes << 4) | k.address_bytes);
        at = helper_put_be(at, k.address, k.address_bytes);
        at = helper_put_be(at, k.blob.size(), k.size_bytes);
    } else if (h.at == helper_stage::block) {
        const uint32_t n = helper_block_bytes(k, h);
        *at++ = h.counter;
        std::memcpy(at, k.blob.data() + h.sent, n);
        at += n;
    } else if (h.at == helper_stage::start) {
        *at++ = 0x01;
        *at++ = static_cast<uint8_t>(k.routine >> 8);
        *at++ = static_cast<uint8_t>(k.routine);
        std::memcpy(at, k.option, k.option_len);
        at += k.option_len;
    }
    return static_cast<uint32_t>(at - out);
}

/* The 74's maxNumberOfBlockLength counts the 36 and its counter */
inline bool helper_opened(helper_run& h, uint32_t wire_max, const uint8_t* d,
                          size_t n)
{
    const size_t sized = n >= 2 ? d[1] >> 4 : 0;
    if (d[0] != 0x74 || sized < 1 || sized > 4 || n != 2 + sized) {
        return false;
    }
    uint64_t most = 0;
    for (size_t i = 0; i < sized; ++i) {
        most = most << 8 | d[2 + i];
    }
    const uint64_t wire = wire_max < kWireMost ? wire_max : kWireMost;
    if (most > wire) {
        most = wire;
    }
    if (most < 3) {
        return false;
    }
    h.block = static_cast<uint32_t>(most - 2);
    h.counter = 1;
    h.sent = 0;
    h.at = helper_stage::block;
    return true;
}

inline helper_took helper_take(const kernel& k, helper_run& h,
                               uint32_t wire_max, const uint8_t* d, size_t n)
{
    switch (h.at) {
    case helper_stage::open:
        return helper_opened(h, wire_max, d, n) ? helper_took::next
                                                : helper_took::bad;
    case helper_stage::block:
        if (n < 2 || d[0] != 0x76 || d[1] != h.counter) {
            return helper_took::bad;
        }
        h.sent += helper_block_bytes(k, h);
        ++h.counter;
        if (h.sent == k.blob.size()) {
            h.at = helper_stage::exit;
        }
        return helper_took::next;
    case helper_stage::exit:
        if (d[0] != 0x77) {
            return helper_took::bad;
        }
        h.at = helper_stage::start;
        return helper_took::next;
    default:
        if (n < 4 || d[0] != 0x71 || d[1] != 0x01 ||
            d[2] != static_cast<uint8_t>(k.routine >> 8) ||
            d[3] != static_cast<uint8_t>(k.routine)) {
            return helper_took::bad;
        }
        h.running = true;
        ++h.starts;
        h.status_given = n > 4;
        h.status = n > 4 ? d[4] : uint8_t{0};
        h.at = helper_stage::open;
        return helper_took::started;
    }
}

} // namespace ecuio_operation

#endif /* ECUIO_OPERATION_KERNEL_READ_H */
