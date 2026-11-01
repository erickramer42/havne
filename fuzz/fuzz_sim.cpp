// File/layer: fuzz/fuzz_sim.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: the simulated ECU (ADR-0046) from arbitrary bytes: the
//          URI's parser on arbitrary text, and the ECU answering a run of
//          arbitrary requests off the bus, its car stepped between them.
//          A reply stays within ECUIO_SIM_REPLY_MAX and is a positive
//          reply to the request's service or a three-byte negative one
//          naming it; a fate is one of five, and two ECUs given the same
//          requests answer the same

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
void run(const ecuio_sim_config& config, const uint8_t* data, size_t size,
         std::string* out)
{
    ecuio_sim_ecu e;
    e.config = config;
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
    std::string a;
    std::string b;
    run(config, data + 1, size - 1, &a);
    run(config, data + 1, size - 1, &b);
    if (a != b) {
        abort();
    }
    return 0;
}
