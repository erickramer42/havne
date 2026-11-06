// File/layer: fuzz/fuzz_sim.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_sim

#include "sim_ecu.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace {

/* The ECU answering the requests in data: each a length byte (0-15)
 * then that many bytes, a tick of the car after each. Its replies'
 * bytes, run together, into out. */
void run(const ecuio_sim_config& config, const ecuio_sim_recording* rec,
         const uint8_t* data, size_t size, std::string* out)
{
    ecuio_sim_ecu e;
    e.config = config;
    e.recording = rec;
    size_t at = 0;
    while (at < size) {
        const size_t len = data[at] & 0x0F;
        const bool functional = (data[at] & 0x10) != 0;
        ++at;
        if (len > size - at) {
            break;
        }
        uint8_t reply[ECUIO_SIM_REPLY_MAX + 16];
        memset(reply, 0xEE, sizeof reply);
        const uint32_t n = ecuio_sim_answer(
            e, data + at, static_cast<uint32_t>(len), functional, reply);
        if (n > ECUIO_SIM_REPLY_MAX || reply[ECUIO_SIM_REPLY_MAX] != 0xEE) {
            abort();
        }
        if (n > 0) {
            const bool negative = reply[0] == 0x7F;
            if (negative ? (n != 3 || reply[1] != data[at])
                         : reply[0] != (data[at] | 0x40)) {
                abort();
            }
            const ecuio_sim_fate fate = ecuio_sim_next_fate(
                e, data + at, static_cast<uint32_t>(len), n);
            if (static_cast<uint8_t>(fate) >
                static_cast<uint8_t>(ecuio_sim_fate::stalled)) {
                abort();
            }
            out->append(reinterpret_cast<const char*>(reply), n);
        }
        at += len;
        ecuio_sim_tick(e);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1 || size > 4096) {
        return 0;
    }
    /* The first byte picks: odd, the URI's parser on the rest as text */
    if (data[0] & 1) {
        std::string text(reinterpret_cast<const char*>(data + 1), size - 1);
        for (char& c : text) {
            if (c == '\0') {
                c = 'x';
            }
        }
        ecuio_sim_config config;
        char why[256];
        memset(why, 0, sizeof why);
        const ecuio_sim_uri_result r =
            ecuio_sim_parse_uri(text.c_str(), &config, why, sizeof why);
        if (r != ecuio_sim_uri_result::ok && why[0] == '\0') {
            abort();
        }
        if (r == ecuio_sim_uri_result::ok &&
            (strlen(config.vin) != ECUIO_VIN_LEN ||
             config.faults.silent > ECUIO_SIM_FAULT_MAX ||
             config.faults.late > ECUIO_SIM_FAULT_MAX ||
             config.faults.bad_sequence > ECUIO_SIM_FAULT_MAX ||
             config.faults.stalled > ECUIO_SIM_FAULT_MAX)) {
            abort();
        }
        return 0;
    }
    /* Even: the ECU, its mode and faults from the first byte */
    ecuio_sim_config config;
    memcpy(config.vin, ECUIO_SIM_DEFAULT_VIN, sizeof ECUIO_SIM_DEFAULT_VIN);
    config.values =
        (data[0] & 2) ? ecuio_sim_values::canned : ecuio_sim_values::drive;
    config.faults.silent = (data[0] >> 2) & 3;
    config.faults.late = (data[0] >> 4) & 3;
    config.faults.bad_sequence = (data[0] >> 6) & 1;
    config.faults.stalled = (data[0] >> 7) & 1;
    ++data;
    --size;
    /* 'R' next: a recording first, then the requests. Its frames: a
     * count byte, then each a length byte (0-15), a flags byte (bit 0 an
     * ID other than the engine ECU's, the rest the frame's flags' low
     * bits), a step in 100 ms and its bytes */
    ecuio_sim_recording rec;
    const ecuio_sim_recording* source = nullptr;
    if (size > 1 && data[0] == 'R') {
        size_t at = 2;
        const unsigned frames = data[1];
        ecuio_frame_t* f = new ecuio_frame_t();
        int64_t t = 0;
        for (unsigned k = 0; k < frames && at + 3 <= size; ++k) {
            const size_t len = data[at] & 0x0F;
            if (len > size - at - 3) {
                break;
            }
            f->arbitration_id = (data[at + 1] & 1) ? 0x7E9u : 0x7E8u;
            f->flags = data[at + 1] & 0xF0u;
            t += static_cast<int64_t>(data[at + 2]) * 100000;
            f->data_length = static_cast<uint16_t>(len);
            memcpy(f->data, data + at + 3, len);
            ecuio_sim_recording_take(rec, t, *f);
            at += 3 + len;
        }
        delete f;
        rec.loop = (config.faults.stalled & 1) != 0;
        if (rec.span_us < 0) {
            abort();
        }
        for (const auto& kv : rec.replies) {
            for (const ecuio_sim_take& take : kv.second) {
                if (take.offset + take.len > rec.bytes.size() ||
                    take.len > ECUIO_SIM_RECORDING_REPLY_MAX) {
                    abort();
                }
            }
        }
        source = &rec;
        data += at < size ? at : size;
        size -= at < size ? at : size;
    }
    std::string a;
    std::string b;
    run(config, source, data, size, &a);
    run(config, source, data, size, &b);
    if (a != b) {
        abort();
    }
    return 0;
}
