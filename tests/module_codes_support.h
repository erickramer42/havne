#ifndef ECUIO_TESTS_MODULE_CODES_SUPPORT_H
#define ECUIO_TESTS_MODULE_CODES_SUPPORT_H

#include <cstdint>
#include <vector>

namespace module_codes_test {

struct Code {
    uint32_t code;
    uint8_t status;
};

inline const std::vector<Code>& engine_codes()
{
    static const std::vector<Code> codes = {
        {0x012800, 0x20}, {0xD12345, 0x24}, {0x9A011C, 0x89},
        {0x030100, 0x08}, {0x056311, 0x50},
    };
    return codes;
}

inline std::vector<Code> codes_under(uint8_t mask)
{
    std::vector<Code> out;
    for (const Code& c : engine_codes()) {
        if ((c.status & mask) != 0) {
            out.push_back(c);
        }
    }
    return out;
}

/* 59 01 FF 00 00 n and 59 02 FF then the codes under the mask; {} for any
 * other request */
inline std::vector<uint8_t> codes_reply(const std::vector<uint8_t>& q)
{
    if (q.size() != 3 || q[0] != 0x19 || (q[1] != 0x01 && q[1] != 0x02)) {
        return {};
    }
    const std::vector<Code> held = codes_under(q[2]);
    if (q[1] == 0x01) {
        return {0x59, 0x01, 0xFF,
                0x00, 0x00, static_cast<uint8_t>(held.size())};
    }
    std::vector<uint8_t> r = {0x59, 0x02, 0xFF};
    for (const Code& c : held) {
        r.push_back(static_cast<uint8_t>(c.code >> 16));
        r.push_back(static_cast<uint8_t>(c.code >> 8));
        r.push_back(static_cast<uint8_t>(c.code));
        r.push_back(c.status);
    }
    return r;
}

} // namespace module_codes_test

#endif
