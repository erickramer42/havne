// File/layer: fuzz/fuzz_sim.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_sim

#include "fuzz_sim_gateway.h"
#include "sim_ecu.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace {

/* A timing the URI took: within its bounds, and every module's draw
 * (each aim's too) from 0 to the ceiling */
void check_one(const ecuio_sim_timing::sim_timing& t)
{
    using namespace ecuio_sim_timing;
    if (t.fc_us > GAP_MAX_US || t.cf_us > GAP_MAX_US || t.p50_us < 0 ||
        t.busy > busy_mode::nrc21 || t.p99_us > DELAY_MAX_US ||
        (t.p50_us > 0 && t.p50_us > t.p99_us)) {
        abort();
    }
    std::mt19937_64 rng = sim_timing_rng(t.seed, 0x7E8);
    for (int i = 0; i < 4; ++i) {
        const int64_t d = sim_timing_draw(t, rng);
        if (d < 0 || d > static_cast<int64_t>(DRAW_MAX_US) ||
            (t.p50_us == 0 && d != 0)) {
            abort();
        }
    }
}

void check_timing(const ecuio_sim_timing::timings& t)
{
    using ecuio_sim_timing::AIMED_KEYS;
    using ecuio_sim_timing::AIMED_MAX;
    uint32_t aimed = 0;
    for (uint32_t k = 0; k < AIMED_KEYS; ++k) {
        if (t.aimed[k] > AIMED_MAX) {
            abort();
        }
        aimed += t.aimed[k];
    }
    if (t.count != aimed || t.bus_kbps > ecuio_sim_timing::BUS_MAX_KBPS) {
        abort();
    }
    check_one(t.all);
    for (uint32_t i = 0; i < t.count; ++i) {
        if (t.items[i].reply_id > 0x7FFu) {
            abort();
        }
        check_one(ecuio_sim_timing::for_node(t, t.items[i].reply_id));
    }
}

/* The extended or the programming session: where security and memory
 * answer */
bool in_session(const ecuio_sim_session& s)
{
    return s.extended || s.programming;
}

/* What the node's session and security access must hold after a reply
 * to req: a key is accepted only when it is its level's own, in the
 * extended session, and the gated DID answers only while unlocked */
void check_security(const ecuio_sim_session& s, const uint8_t* req, size_t len,
                    const uint8_t* reply, uint32_t n)
{
    if (n >= 2 && reply[0] == 0x67 && reply[1] % 2 == 0) {
        const ecuio_sim_security_level* l =
            ecuio_sim_security_find(static_cast<uint8_t>(reply[1] - 1));
        uint8_t want[sizeof l->seed];
        if (l == nullptr || !in_session(s) || len != 2u + l->bytes) {
            abort();
        }
        ecuio_sim_security_key(l->seed, l->bytes, want);
        if (memcmp(req + 2, want, l->bytes) != 0 ||
            s.security.unlocked != l->level) {
            abort();
        }
    }
    if (n >= 3 && reply[0] == 0x62 && reply[1] == 0xD2 && reply[2] == 0x10 &&
        (!in_session(s) || s.security.unlocked == 0)) {
        abort();
    }
    if (s.extended && s.programming) {
        abort();
    }
    if (s.security.unlocked != 0 && !in_session(s)) {
        abort();
    }
}

/* What an upload's replies must hold, was the memory's state before the
 * request: a 75 opens one only while none is open, a 76 hands over the
 * open one's next bytes under its counter and never past what was
 * asked, and a 77 ends one with nothing left */
void check_upload(const ecuio_sim_session& s, const ecuio_sim_memory& was,
                  const uint8_t* reply, uint32_t n)
{
    const bool open = in_session(s) && s.security.unlocked != 0;
    if (n >= 1 && reply[0] == 0x75 &&
        (!open || was.uploading || n != 4 || !s.memory.uploading ||
         s.memory.left == 0)) {
        abort();
    }
    if (n >= 1 && reply[0] == 0x76) {
        if (!open || !was.uploading || n < 3 || n > was.faults.block ||
            n - 2 > was.left || reply[1] != was.counter ||
            s.memory.left != was.left - (n - 2)) {
            abort();
        }
        for (uint32_t i = 0; i + 2 < n; ++i) {
            if (reply[2 + i] != ecuio_sim_memory_byte(s.memory, was.next + i)) {
                abort();
            }
        }
    }
    if (n >= 1 && reply[0] == 0x77 &&
        (!in_session(s) || !was.uploading || was.left != 0 || n != 1)) {
        abort();
    }
    if (s.memory.uploading && !in_session(s)) {
        abort();
    }
}

/* What the node's memory must hold after a reply to req: a region's
 * bytes are answered only in the extended session while unlocked, to a
 * read inside the region, and are the region's own at that address; an
 * identification DID answers only in the session */
void check_memory(const ecuio_sim_session& s, const uint8_t* req, size_t len,
                  const uint8_t* reply, uint32_t n)
{
    if (n >= 1 && reply[0] == 0x63) {
        if (!in_session(s) || s.security.unlocked == 0 || len < 4 ||
            req[0] != 0x23) {
            abort();
        }
        const size_t a = req[1] & 0x0F;
        const size_t z = req[1] >> 4;
        if (a < 1 || a > 4 || z < 1 || z > 4 || len != 2 + a + z) {
            abort();
        }
        uint64_t address = 0;
        uint64_t size = 0;
        for (size_t i = 0; i < a; ++i) {
            address = (address << 8) | req[2 + i];
        }
        for (size_t i = 0; i < z; ++i) {
            size = (size << 8) | req[2 + a + i];
        }
        const auto inside = [&](uint64_t from, uint64_t bytes) {
            return address >= from && address + size <= from + bytes;
        };
        const bool code = s.programming && inside(ECUIO_SIM_MEMORY_CODE_ADDRESS,
                                                  ECUIO_SIM_MEMORY_CODE_BYTES);
        if (size == 0 || n != 1 + size ||
            (!inside(ECUIO_SIM_MEMORY_ADDRESS, ECUIO_SIM_MEMORY_BYTES) &&
             !code)) {
            abort();
        }
        for (uint64_t i = 0; i < size; ++i) {
            if (reply[1 + i] !=
                ecuio_sim_memory_byte(s.memory,
                                      static_cast<uint32_t>(address + i))) {
                abort();
            }
        }
    }
    if (n >= 3 && reply[0] == 0x62 && reply[1] == 0xD2 &&
        (reply[2] == 0x20 || reply[2] == 0x21) && !in_session(s)) {
        abort();
    }
}

void check_kernel(const ecuio_sim_session& s, bool was_running,
                  const uint8_t* req, size_t len, const uint8_t* reply,
                  uint32_t n)
{
    const ecuio_sim_kernel& k = s.kernel;
    if (!k.on) {
        if (k.running || k.downloading || (n >= 1 && reply[0] == 0x74)) {
            abort();
        }
        return;
    }
    if (n >= 1 && reply[0] == 0x71 &&
        (!k.running || k.faults.storm || k.faults.refuse)) {
        abort();
    }
    if (n >= 1 && (reply[0] == 0x63 || reply[0] == 0x75) && s.programming &&
        ecuio_sim_memory_asks_code(req, static_cast<uint32_t>(len)) &&
        !was_running) {
        abort();
    }
}

/* The ECU answering the requests in data as a node on a bus does: each
 * a length byte (bits 0-3, and bit 5 for 16 more) then that many bytes,
 * a tick of the car after each; with no recording the node has a
 * session, its security access and its memory (sim.cpp's order: the
 * session's services, then the ECU's); a functional request changes
 * none of them. Its replies' bytes, run together, into out. */
void run(const ecuio_sim_config& config, const ecuio_sim_recording* rec,
         const uint8_t* data, size_t size, std::string* out)
{
    ecuio_sim_ecu e;
    e.config = config;
    e.recording = rec;
    ecuio_sim_session session;
    session.memory.vin = rec == nullptr ? e.config.vin : nullptr;
    session.kernel.on = config.kernel;
    session.kernel.faults = config.kernel_faults;
    ecuio_sim_counts counts;
    size_t at = 0;
    while (at < size) {
        const size_t len = (data[at] & 0x0F) + ((data[at] & 0x20) ? 16u : 0u);
        const bool functional = (data[at] & 0x10) != 0;
        ++at;
        if (len > size - at) {
            break;
        }
        uint8_t reply[ECUIO_SIM_REPLY_MAX + 16];
        memset(reply, 0xEE, sizeof reply);
        uint32_t n = 0;
        bool handled = false;
        const uint8_t unlocked = session.security.unlocked;
        const uint8_t seed_out = session.security.seed_out;
        const bool extended = session.extended;
        const bool programming = session.programming;
        const ecuio_sim_memory was = session.memory;
        const bool running = session.kernel.running;
        const bool to_kernel =
            len > 0 && ecuio_sim_kernel_service(session.kernel, data + at);
        if (!functional && rec == nullptr) {
            n = ecuio_sim_session_answer(session, true, true, e.time_us,
                                         data + at, static_cast<uint32_t>(len),
                                         reply, &handled);
        }
        if (!handled) {
            n = ecuio_sim_answer(e, session, data + at,
                                 static_cast<uint32_t>(len), functional, reply);
        }
        if (n > ECUIO_SIM_REPLY_MAX || reply[ECUIO_SIM_REPLY_MAX] != 0xEE) {
            abort();
        }
        if (rec == nullptr) {
            check_security(session, data + at, len, reply, n);
            check_memory(session, data + at, len, reply, n);
            if (!to_kernel) {
                check_upload(session, was, reply, n);
            }
            check_kernel(session, running, data + at, len, reply, n);
        }
        if (functional && (session.security.unlocked != unlocked ||
                           session.security.seed_out != seed_out ||
                           session.extended != extended ||
                           session.programming != programming ||
                           session.memory.uploading != was.uploading ||
                           session.memory.left != was.left ||
                           (n > 0 && (reply[0] == 0x63 || reply[0] == 0x75 ||
                                      reply[0] == 0x76 || reply[0] == 0x77)))) {
            abort();
        }
        if (n > 0) {
            const bool negative = reply[0] == 0x7F;
            if (negative ? (n != 3 || reply[1] != data[at])
                         : reply[0] != (data[at] | 0x40)) {
                abort();
            }
            const ecuio_sim_fate fate =
                ecuio_sim_next_fate(e.config.faults, counts, data + at,
                                    static_cast<uint32_t>(len), n);
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
             config.faults.stalled > ECUIO_SIM_FAULT_MAX ||
             config.aimed.count > ECUIO_SIM_AIMED_MAX)) {
            abort();
        }
        if (r == ecuio_sim_uri_result::ok) {
            fuzz_sim_gateway::check_config(config);
            check_timing(config.timing);
            const ecuio_sim_memory_faults& m = config.memory;
            const uint32_t periods[] = {m.cut, m.flip,   m.relock,
                                        m.seq, m.refuse, m.ident};
            for (const uint32_t every : periods) {
                if (every > ECUIO_SIM_MEMORY_FAULT_MAX) {
                    abort();
                }
            }
            if (m.block == 0 || m.block > ECUIO_SIM_MEMORY_BLOCK_MAX ||
                m.moving_at >= ECUIO_SIM_MEMORY_BYTES ||
                (m.moving_at != 0 && !m.moving)) {
                abort();
            }
        }
        /* Each aim one 11-bit module's, its period as a fault's */
        for (uint32_t i = 0;
             r == ecuio_sim_uri_result::ok && i < config.aimed.count; ++i) {
            const ecuio_sim_aim& a = config.aimed.items[i];
            if (a.reply_id > 0x7FFu || a.every == 0 ||
                a.every > ECUIO_SIM_FAULT_MAX) {
                abort();
            }
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
    if (size > 0 && data[0] == 'G') {
        fuzz_sim_gateway::fuzz(data, size);
        return 0;
    }
    if (size > 1 && data[0] == 'K') {
        config.kernel = true;
        config.kernel_faults.once = (data[1] & 1) != 0;
        config.kernel_faults.storm = (data[1] & 2) != 0;
        config.kernel_faults.silent = (data[1] & 4) != 0;
        config.kernel_faults.refuse = (data[1] & 8) != 0;
        data += 2;
        size -= 2;
    }
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
