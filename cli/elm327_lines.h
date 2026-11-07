// File/layer: cli/elm327_lines.h | havne-cli, the ELM327 emulator's
//             address and lines (ADR-0055); public headers only
// Testing: UNIT: CliElm327Test (test_cli)
//
// `havne-cli sim <uri> --elm327 tcp:[<address>:]<port>`: where the
// emulator listens (cli/elm327_listener.h), 127.0.0.1 unless the user
// names another IPv4 address (a listener the LAN can reach is the
// user's choice), port 0 for one the system picks. Lines: `elm327`
// {uri, listen} once listening, `client` {event connected|closed|
// refused, peer}.

#ifndef HAVNE_CLI_ELM327_LINES_H
#define HAVNE_CLI_ELM327_LINES_H

#include "jsonl.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace cli {

/* Where the emulator listens: an IPv4 address (host order) and a port */
struct Elm327Address {
    uint32_t ip = 0x7F000001u; /* 127.0.0.1 */
    uint16_t port = 0;
};

namespace elm327_detail {

/* A decimal number of 1 to max_digits digits, no leading zero, at most
 * max; false otherwise */
inline bool decimal(const std::string& s, uint32_t max, uint32_t* out)
{
    if (s.empty() || s.size() > 5 || (s.size() > 1 && s[0] == '0')) {
        return false;
    }
    uint32_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<uint32_t>(c - '0');
    }
    if (v > max) {
        return false;
    }
    *out = v;
    return true;
}

} // namespace elm327_detail

/* "tcp:<port>" (127.0.0.1) or "tcp:<a.b.c.d>:<port>": four decimal
 * octets, a port 0-65535 (0: the system picks), no leading zeros, no
 * host names; false otherwise */
inline bool parse_elm327_address(const char* text, Elm327Address* out)
{
    if (text == nullptr || std::strncmp(text, "tcp:", 4) != 0) {
        return false;
    }
    const std::string rest(text + 4);
    const size_t colon = rest.rfind(':');
    Elm327Address a;
    uint32_t port = 0;
    if (!elm327_detail::decimal(
            colon == std::string::npos ? rest : rest.substr(colon + 1), 65535,
            &port)) {
        return false;
    }
    a.port = static_cast<uint16_t>(port);
    if (colon != std::string::npos) {
        const std::string host = rest.substr(0, colon);
        uint32_t ip = 0;
        size_t at = 0;
        for (int i = 0; i < 4; ++i) {
            const size_t dot = i < 3 ? host.find('.', at) : host.size();
            if (dot == std::string::npos) {
                return false;
            }
            uint32_t octet = 0;
            if (!elm327_detail::decimal(host.substr(at, dot - at), 255,
                                        &octet)) {
                return false;
            }
            ip = (ip << 8) | octet;
            at = dot + 1;
        }
        if (at != host.size() + 1) {
            return false;
        }
        a.ip = ip;
    }
    *out = a;
    return true;
}

/* "a.b.c.d:port" */
inline std::string address_text(uint32_t ip, uint16_t port)
{
    char b[32];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u:%u", (ip >> 24) & 0xFFu,
                  (ip >> 16) & 0xFFu, (ip >> 8) & 0xFFu, ip & 0xFFu,
                  static_cast<unsigned>(port));
    return b;
}

/* {"type":"elm327", uri, listen}: the emulator is listening */
inline void elm327_line(Line& l, const char* uri, const std::string& listen)
{
    l.raw("{\"type\":\"elm327\"");
    l.key("uri");
    l.string(uri, SIZE_MAX, 256);
    l.key("listen");
    l.string(listen.c_str(), SIZE_MAX, 32);
    l.ch('}');
}

/* {"type":"client", event, peer}: connected, closed or refused */
inline void client_line(Line& l, const char* event, const std::string& peer)
{
    l.raw("{\"type\":\"client\"");
    l.key("event");
    l.string(event, SIZE_MAX);
    l.key("peer");
    l.string(peer.c_str(), SIZE_MAX, 32);
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_ELM327_LINES_H
