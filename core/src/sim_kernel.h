#ifndef ECUIO_SIM_KERNEL_H
#define ECUIO_SIM_KERNEL_H

#include "sim_memory.h"
#include "sim_security.h"

#include <cstdint>
#include <cstring>

constexpr uint32_t ECUIO_SIM_KERNEL_RAM_ADDRESS = 0x00F00000u;
constexpr uint32_t ECUIO_SIM_KERNEL_RAM_BYTES = 0x00004000u;
constexpr uint16_t ECUIO_SIM_KERNEL_ROUTINE = 0x0201;
constexpr uint16_t ECUIO_SIM_KERNEL_BLOCK = 0x0402;
constexpr char ECUIO_SIM_KERNEL_MAGIC[] = "HAVNEKRN";
constexpr uint32_t ECUIO_SIM_KERNEL_MAGIC_BYTES = 8;
constexpr uint32_t ECUIO_SIM_KERNEL_HEAD = ECUIO_SIM_KERNEL_MAGIC_BYTES + 4;
constexpr uint8_t ECUIO_SIM_NRC_PENDING = 0x78;

struct ecuio_sim_kernel_faults {
    bool once = false;   /* the helper ends at an upload's 37 */
    bool storm = false;  /* the start answers 7F 31 78 and nothing more */
    bool silent = false; /* once started, the module answers nothing */
    bool refuse = false; /* the start answers 7F 31 22 whatever it holds */
};

struct ecuio_sim_kernel {
    bool on = false;
    ecuio_sim_kernel_faults faults;
    bool downloading = false;
    uint32_t next = 0;
    uint32_t left = 0;
    uint8_t counter = 1;
    uint32_t got = 0;
    uint8_t head[ECUIO_SIM_KERNEL_HEAD] = {};
    uint32_t sum = 0;
    bool loaded = false;
    bool running = false;
    uint32_t downloads = 0;
    uint32_t starts = 0;
};

inline void ecuio_sim_kernel_end(ecuio_sim_kernel& k)
{
    const bool on = k.on;
    const ecuio_sim_kernel_faults f = k.faults;
    const uint32_t downloads = k.downloads;
    const uint32_t starts = k.starts;
    k = ecuio_sim_kernel{};
    k.on = on;
    k.faults = f;
    k.downloads = downloads;
    k.starts = starts;
}

/* What the start accepts: the magic, the big-endian sum of the rest */
inline uint32_t ecuio_sim_kernel_image(uint8_t* out, uint32_t size)
{
    if (size <= ECUIO_SIM_KERNEL_HEAD) {
        return 0;
    }
    std::memcpy(out, ECUIO_SIM_KERNEL_MAGIC, ECUIO_SIM_KERNEL_MAGIC_BYTES);
    uint32_t sum = 0;
    for (uint32_t i = ECUIO_SIM_KERNEL_HEAD; i < size; ++i) {
        out[i] = static_cast<uint8_t>(i * 31u + 7u);
        sum += out[i];
    }
    for (uint32_t i = 0; i < 4; ++i) {
        out[ECUIO_SIM_KERNEL_MAGIC_BYTES + i] =
            static_cast<uint8_t>(sum >> (24 - 8 * i));
    }
    return size;
}

namespace ecuio_sim_kernel_detail {

using ecuio_sim_memory_detail::refuse;
using ecuio_sim_memory_detail::span;

inline uint32_t open_download(ecuio_sim_kernel& k,
                              const ecuio_sim_security& security,
                              const uint8_t* req, uint32_t len, uint8_t* out)
{
    if (len < 4) {
        return refuse(0x34, ECUIO_SIM_NRC_LENGTH, out);
    }
    span s = ecuio_sim_memory_detail::fields(req[2]);
    if (len != 3u + s.address_bytes + s.size_bytes) {
        return refuse(0x34, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (s.address_bytes >= 1 && s.address_bytes <= 4 && s.size_bytes >= 1 &&
        s.size_bytes <= 4) {
        ecuio_sim_memory_detail::read_span(req + 3, &s);
    } else {
        s.size = 0;
    }
    const bool inside = s.size > ECUIO_SIM_KERNEL_HEAD &&
                        s.address >= ECUIO_SIM_KERNEL_RAM_ADDRESS &&
                        s.size <= ECUIO_SIM_KERNEL_RAM_BYTES &&
                        s.address - ECUIO_SIM_KERNEL_RAM_ADDRESS <=
                            ECUIO_SIM_KERNEL_RAM_BYTES - s.size;
    if (req[1] != 0x00 || !inside) {
        return refuse(0x34, ECUIO_SIM_NRC_RANGE, out);
    }
    if (security.unlocked == 0) {
        return refuse(0x34, ECUIO_SIM_NRC_DENIED, out);
    }
    if (k.downloading || k.running) {
        return refuse(0x34, ECUIO_SIM_NRC_UPLOAD, out);
    }
    ecuio_sim_kernel_end(k);
    k.downloading = true;
    k.next = static_cast<uint32_t>(s.address);
    k.left = static_cast<uint32_t>(s.size);
    ++k.downloads;
    out[0] = 0x74;
    out[1] = 0x20;
    out[2] = static_cast<uint8_t>(ECUIO_SIM_KERNEL_BLOCK >> 8);
    out[3] = static_cast<uint8_t>(ECUIO_SIM_KERNEL_BLOCK & 0xFF);
    return 4;
}

inline uint32_t next_block(ecuio_sim_kernel& k,
                           const ecuio_sim_security& security,
                           const uint8_t* req, uint32_t len, uint8_t* out)
{
    const uint32_t n = len > 2 ? len - 2 : 0;
    if (len < 3 || len > ECUIO_SIM_KERNEL_BLOCK || n > k.left) {
        return refuse(0x36, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (security.unlocked == 0) {
        return refuse(0x36, ECUIO_SIM_NRC_DENIED, out);
    }
    if (req[1] != k.counter) {
        return refuse(0x36, ECUIO_SIM_NRC_COUNTER, out);
    }
    for (uint32_t i = 0; i < n; ++i, ++k.got) {
        if (k.got < ECUIO_SIM_KERNEL_HEAD) {
            k.head[k.got] = req[2 + i];
        } else {
            k.sum += req[2 + i];
        }
    }
    k.next += n;
    k.left -= n;
    out[0] = 0x76;
    out[1] = k.counter++;
    return 2;
}

inline uint32_t end_download(ecuio_sim_kernel& k, uint32_t len, uint8_t* out)
{
    if (len != 1) {
        return refuse(0x37, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (k.left != 0) {
        return refuse(0x37, ECUIO_SIM_NRC_SEQUENCE, out);
    }
    k.downloading = false;
    k.loaded = true;
    out[0] = 0x77;
    return 1;
}

inline bool image_holds(const ecuio_sim_kernel& k)
{
    uint32_t want = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        want = (want << 8) | k.head[ECUIO_SIM_KERNEL_MAGIC_BYTES + i];
    }
    return std::memcmp(k.head, ECUIO_SIM_KERNEL_MAGIC,
                       ECUIO_SIM_KERNEL_MAGIC_BYTES) == 0 &&
           want == k.sum;
}

inline uint32_t start(ecuio_sim_kernel& k, const ecuio_sim_security& security,
                      const uint8_t* req, uint32_t len, uint8_t* out)
{
    if (len < 4) {
        return refuse(0x31, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (req[1] != 0x01) {
        return refuse(0x31, ECUIO_SIM_NRC_SUBFUNCTION, out);
    }
    if (((uint32_t{req[2]} << 8) | req[3]) != ECUIO_SIM_KERNEL_ROUTINE) {
        return refuse(0x31, ECUIO_SIM_NRC_RANGE, out);
    }
    if (len != 4) {
        return refuse(0x31, ECUIO_SIM_NRC_LENGTH, out);
    }
    if (security.unlocked == 0) {
        return refuse(0x31, ECUIO_SIM_NRC_DENIED, out);
    }
    if (!k.loaded || k.running) {
        return refuse(0x31, ECUIO_SIM_NRC_SEQUENCE, out);
    }
    ++k.starts;
    if (k.faults.storm) {
        return refuse(0x31, ECUIO_SIM_NRC_PENDING, out);
    }
    if (k.faults.refuse || !image_holds(k)) {
        return refuse(0x31, ECUIO_SIM_NRC_CONDITIONS, out);
    }
    k.running = true;
    out[0] = 0x71;
    out[1] = 0x01;
    out[2] = req[2];
    out[3] = req[3];
    return 4;
}

} // namespace ecuio_sim_kernel_detail

inline bool ecuio_sim_kernel_service(const ecuio_sim_kernel& k,
                                     const uint8_t* req)
{
    return k.on && (req[0] == 0x34 || req[0] == 0x31 ||
                    (k.downloading && (req[0] == 0x36 || req[0] == 0x37)));
}

/* The refusal order is SimKernelTest's; 0: no answer */
inline uint32_t ecuio_sim_kernel_answer(ecuio_sim_kernel& k,
                                        const ecuio_sim_security& security,
                                        bool programming, const uint8_t* req,
                                        uint32_t len, uint8_t* out)
{
    namespace detail = ecuio_sim_kernel_detail;
    if (k.running && k.faults.silent) {
        return 0;
    }
    if (!programming) {
        return detail::refuse(req[0], ECUIO_SIM_NRC_SESSION, out);
    }
    switch (req[0]) {
    case 0x34:
        return detail::open_download(k, security, req, len, out);
    case 0x36:
        return detail::next_block(k, security, req, len, out);
    case 0x37:
        return detail::end_download(k, len, out);
    default:
        return detail::start(k, security, req, len, out);
    }
}

inline void ecuio_sim_kernel_upload_done(ecuio_sim_kernel& k)
{
    if (k.faults.once) {
        ecuio_sim_kernel_end(k);
    }
}

#endif /* ECUIO_SIM_KERNEL_H */
