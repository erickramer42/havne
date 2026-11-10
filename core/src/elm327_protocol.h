// File/layer: core/src/elm327_protocol.h | internal, not ABI-exported;
//             the ELM327 wire (ADR-0026 section 3), for the ELM327 transport
// C++17, Qt-free, no platform headers. No I/O, no clock, no allocation.
// Testing: UNIT: Elm327ProtocolTest (test_elm327)

#ifndef ECUIO_ELM327_PROTOCOL_H
#define ECUIO_ELM327_PROTOCOL_H

#include "ecuio/core.h"    /* ECUIO_FRAME_DATA_MAX */
#include "ecuio/signals.h" /* the defs the count digit is chosen from */
#include "isotp.h"         /* a frame line's messages */
#include "obd2_reply.h"    /* J1979's record length per PID */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The sources: the ELM327 datasheet (Elm Electronics, ELM327DSH, v1.4b),
 * the OBDLink Family Reference and Programming Manual (rev E,
 * 2025-01-10) and the OBDX Pro Command Set Reference Guide (3.00); what
 * each says is summarized in ADR-0026's Context. */

/* ---- Lines ----------------------------------------------------------
 * The adapter ends each line with CR, adds LF after it only under
 * AT L1 (the OBDX's default), and prints '>' when it's ready for the
 * next command, usually after a blank line. Some adapters send NUL
 * bytes around a reset. Bytes arrive in any chunking. */

/* The longest line kept. The longest the adapter sends is a 29-bit
 * frame with spaces (8 + 8 * 3 characters) or a version string; a
 * longer one is kept to this length and flagged. */
constexpr size_t ELM327_LINE_MAX = 160;

struct elm327_reader {
    char line[ELM327_LINE_MAX + 1] = {};
    size_t len = 0;
    /* The line ran past ELM327_LINE_MAX; its tail was dropped. */
    bool overflow = false;
    /* line holds a whole line the caller has been given; the next byte
     * starts another. */
    bool done = false;
};

/* What one byte completed: a line (in reader.line, NUL-terminated, valid
 * until the next byte), the prompt, or both, when the prompt follows text
 * with no CR between them. Empty lines are skipped. */
struct elm327_step {
    bool line = false;
    bool prompt = false;
};

inline elm327_step elm327_take(elm327_reader& r, uint8_t b)
{
    elm327_step s;
    if (r.done) {
        r.len = 0;
        r.overflow = false;
        r.line[0] = '\0';
        r.done = false;
    }
    if (b == '\r' || b == '>') {
        if (r.len > 0 || r.overflow) {
            r.line[r.len] = '\0';
            r.done = true;
            s.line = true;
        }
        s.prompt = (b == '>');
        return s;
    }
    if (b == '\n' || b == '\0') {
        return s;
    }
    if (r.len < ELM327_LINE_MAX) {
        r.line[r.len++] = static_cast<char>(b);
    } else {
        r.overflow = true;
    }
    return s;
}

/* A line without spaces, in upper case: how every rule below reads it.
 * Spaces between bytes are a setting (AT S0/S1) that clones don't all
 * honor, and case differs between adapters. out holds at least n + 1. */
inline size_t elm327_normalize(const char* in, size_t n, char* out)
{
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = in[i];
        if (c == ' ' || c == '\t') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        out[k++] = c;
    }
    out[k] = '\0';
    return k;
}

inline int elm327_hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* ---- Frame lines ----------------------------------------------------
 * With AT H1 each received CAN frame is one line: the ID, then the data
 * bytes as received, the ISO-TP PCI byte first and any padding after.
 * With spaces off nothing marks where the ID ends, so the addressing
 * mode decides: eight hex digits for 29-bit; for 11-bit three, or four
 * with a leading 0 (an adapter that prints the ID as two bytes). Data
 * bytes are whole pairs, so an 11-bit line's length tells the two forms
 * apart: odd for three digits, even for four. */

/* A frame line is a CAN frame, its data bytes 1 to 8, as isotp.h
 * takes one. */
using elm327_can_line = isotp_can;

/* norm is a normalized line (elm327_normalize). False for anything that
 * isn't a frame of the mode's shape; out is written only on true. */
inline bool elm327_parse_can(const char* norm, size_t n, bool ext29,
                             elm327_can_line* out)
{
    size_t id_digits = 0;
    if (ext29) {
        id_digits = 8;
    } else if (n % 2 == 1) {
        id_digits = 3;
    } else {
        id_digits = 4;
        if (n < 1 || norm[0] != '0') {
            return false;
        }
    }
    if (n <= id_digits || (n - id_digits) % 2 != 0) {
        return false;
    }
    const size_t bytes = (n - id_digits) / 2;
    if (bytes < 1 || bytes > 8) {
        return false;
    }
    uint32_t id = 0;
    for (size_t i = 0; i < id_digits; ++i) {
        const int v = elm327_hex_value(norm[i]);
        if (v < 0) {
            return false;
        }
        id = (id << 4) | static_cast<uint32_t>(v);
    }
    if (id > (ext29 ? 0x1FFFFFFFu : 0x7FFu)) {
        return false;
    }
    elm327_can_line f;
    f.id = id;
    f.len = static_cast<uint8_t>(bytes);
    for (size_t b = 0; b < bytes; ++b) {
        const int hi = elm327_hex_value(norm[id_digits + 2 * b]);
        const int lo = elm327_hex_value(norm[id_digits + 2 * b + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        f.data[b] = static_cast<uint8_t>((hi << 4) | lo);
    }
    *out = f;
    return true;
}

/* ---- Line kinds -----------------------------------------------------
 * What a line in a request's reply is (ADR-0026 section 6 maps each to the
 * link). The words are matched without spaces and in any case, so a
 * clone that prints NODATA under AT S0 reads as an ELM327's NO DATA. */
enum class elm327_line : uint8_t {
    frame,       /* a CAN frame of the mode's shape */
    ok,          /* OK */
    no_data,     /* NO DATA: the adapter's timer ran out with no reply */
    no_bus,      /* CAN ERROR, BUS ERROR, BUS BUSY, FB ERROR, UNABLE TO
                  * CONNECT, ERRxx: no ACK, or no CAN bus */
    buffer_full, /* BUFFER FULL: data was lost */
    stopped,     /* STOPPED: a character interrupted the adapter */
    refused,     /* ?: a command it doesn't take */
    read_error,  /* DATA ERROR, <DATA ERROR, <RX ERROR, FC RX TIMEOUT,
                  * OUT OF MEMORY, UART RX OVERFLOW: this read failed */
    reset,       /* LV RESET, or a version string: the adapter reset */
    alert,       /* ACT ALERT, LP ALERT: low power is coming */
    searching,   /* SEARCHING...: an automatic protocol search */
    malformed,   /* hex but not a frame of the mode's shape, or a line
                  * longer than ELM327_LINE_MAX */
    text         /* anything else */
};

/* Case-insensitive prefix test, after leading spaces. */
inline bool elm327_starts_with(const char* s, size_t n, const char* prefix)
{
    size_t i = 0;
    while (i < n && s[i] == ' ') {
        ++i;
    }
    for (size_t k = 0; prefix[k] != '\0'; ++k, ++i) {
        if (i >= n) {
            return false;
        }
        char c = s[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        if (c != prefix[k]) {
            return false;
        }
    }
    return true;
}

/* An ELM327 version string: "ELM327" first, in any case. The version
 * after it is never compared: the adapters on hand say v1.4b, V1.4 and
 * v2.1 for the same command set. */
inline bool elm327_is_version(const char* s, size_t n)
{
    return elm327_starts_with(s, n, "ELM327");
}

/* An STN chip's firmware string (STI: "STN<device_id> vX.Y.Z"). */
inline bool elm327_is_stn(const char* s, size_t n)
{
    if (!elm327_starts_with(s, n, "STN")) {
        return false;
    }
    size_t i = 0;
    while (i < n && s[i] == ' ') {
        ++i;
    }
    return i + 3 < n && s[i + 3] >= '0' && s[i + 3] <= '9';
}

/* line is as the reader gave it. A frame line is parsed into *frame
 * (when not NULL). An ATDPN answer or any other short hex value reads as
 * malformed here: a command's reader takes the line as text. */
inline elm327_line elm327_classify(const char* line, size_t n, bool overflow,
                                   bool ext29, elm327_can_line* frame)
{
    if (overflow) {
        return elm327_line::malformed;
    }
    char norm[ELM327_LINE_MAX + 1];
    const size_t k =
        elm327_normalize(line, n < ELM327_LINE_MAX ? n : ELM327_LINE_MAX, norm);
    /* <DATA ERROR and <RX ERROR, alone or beside a frame's bytes */
    if (memchr(norm, '<', k) != nullptr) {
        return elm327_line::read_error;
    }
    struct word {
        const char* text;
        elm327_line kind;
    };
    static const word words[] = {
        {"OK", elm327_line::ok},
        {"NODATA", elm327_line::no_data},
        {"CANERROR", elm327_line::no_bus},
        {"BUSERROR", elm327_line::no_bus},
        {"BUSBUSY", elm327_line::no_bus},
        {"FBERROR", elm327_line::no_bus},
        {"UNABLETOCONNECT", elm327_line::no_bus},
        {"BUFFERFULL", elm327_line::buffer_full},
        {"STOPPED", elm327_line::stopped},
        {"?", elm327_line::refused},
        {"DATAERROR", elm327_line::read_error},
        {"FCRXTIMEOUT", elm327_line::read_error},
        {"OUTOFMEMORY", elm327_line::read_error},
        {"UARTRXOVERFLOW", elm327_line::read_error},
        {"LVRESET", elm327_line::reset},
        {"ACTALERT", elm327_line::alert},
        {"LPALERT", elm327_line::alert},
    };
    for (const word& w : words) {
        if (strcmp(norm, w.text) == 0) {
            return w.kind;
        }
    }
    if (k == 5 && norm[0] == 'E' && norm[1] == 'R' && norm[2] == 'R' &&
        elm327_hex_value(norm[3]) >= 0 && elm327_hex_value(norm[4]) >= 0) {
        return elm327_line::no_bus; /* ERRxx, ERR94 among them */
    }
    if (k >= 9 && strncmp(norm, "SEARCHING", 9) == 0) {
        return elm327_line::searching;
    }
    if (elm327_is_version(norm, k)) {
        return elm327_line::reset;
    }
    bool all_hex = k > 0;
    for (size_t i = 0; i < k && all_hex; ++i) {
        all_hex = elm327_hex_value(norm[i]) >= 0;
    }
    if (!all_hex) {
        return elm327_line::text;
    }
    elm327_can_line f;
    if (!elm327_parse_can(norm, k, ext29, &f)) {
        return elm327_line::malformed;
    }
    if (frame != nullptr) {
        *frame = f;
    }
    return elm327_line::frame;
}

inline elm327_line elm327_classify(const elm327_reader& r, bool ext29,
                                   elm327_can_line* frame)
{
    return elm327_classify(r.line, r.len, r.overflow, ext29, frame);
}

/* The command echoed back (echo is on after every reset until AT E0
 * takes): the line is the command sent, compared without spaces and in
 * any case. sent has no CR. */
inline bool elm327_is_echo(const char* line, size_t n, const char* sent)
{
    char a[ELM327_LINE_MAX + 1];
    char b[ELM327_LINE_MAX + 1];
    const size_t sn = strlen(sent);
    if (n > ELM327_LINE_MAX || sn > ELM327_LINE_MAX) {
        return false;
    }
    const size_t ka = elm327_normalize(line, n, a);
    const size_t kb = elm327_normalize(sent, sn, b);
    return ka == kb && memcmp(a, b, ka) == 0;
}

/* ---- ISO-TP reassembly ----------------------------------------------
 * isotp.h's receiver, untimed: lines come in a burst at the prompt, which
 * ends a reply (isotp_end_all). Per CAN ID, because two ECUs' replies
 * can interleave line by line (the datasheet's 09 04 example). Flow
 * Control frames are the adapter's business (AT CFC1). */

/* ---- Values ---------------------------------------------------------
 * The voltage, from AT RV ("12.6V" on an ELM327, "14.5v" on an OBDX) or
 * STVR ("12.34"), in millivolts. Digits, an optional point and up to
 * three decimals, then an optional V in either case; nothing else, so
 * the dashes an STN prints past 65.5 V, or anything unparsable, are no
 * reading. Parsed digit by digit: strtod follows the locale, which Qt
 * sets. */
inline bool elm327_parse_millivolts(const char* s, size_t n, uint32_t* out_mv)
{
    size_t i = 0;
    while (i < n && s[i] == ' ') {
        ++i;
    }
    while (n > i && s[n - 1] == ' ') {
        --n;
    }
    if (n > i && (s[n - 1] == 'V' || s[n - 1] == 'v')) {
        --n;
    }
    uint32_t whole = 0;
    size_t digits = 0;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; ++i, ++digits) {
        if (digits == 3) {
            return false; /* 1000 V: not a reading */
        }
        whole = whole * 10 + static_cast<uint32_t>(s[i] - '0');
    }
    if (digits == 0) {
        return false;
    }
    uint32_t frac = 0;
    if (i < n && s[i] == '.') {
        ++i;
        size_t places = 0;
        for (; i < n && s[i] >= '0' && s[i] <= '9'; ++i, ++places) {
            if (places == 3) {
                return false;
            }
            frac = frac * 10 + static_cast<uint32_t>(s[i] - '0');
        }
        if (places == 0) {
            return false;
        }
        for (; places < 3; ++places) {
            frac *= 10;
        }
    }
    if (i != n) {
        return false;
    }
    *out_mv = whole * 1000 + frac;
    return true;
}

/* A programmable parameter from one line of AT PPS, which lists them
 * four to a line as <pp>:<value> <state>, the state N (on: the value
 * applies) or F (off: the default does), as in "28:FF F  29:FF F
 * 2A:04 N  2B:02 F". Without spaces each entry is six characters. False
 * when the line doesn't list pp, or isn't in that form. */
inline bool elm327_pps_value(const char* s, size_t n, uint8_t pp,
                             uint8_t* out_value, bool* out_on)
{
    char norm[ELM327_LINE_MAX + 1];
    const size_t k =
        elm327_normalize(s, n < ELM327_LINE_MAX ? n : ELM327_LINE_MAX, norm);
    if (k == 0 || k % 6 != 0) {
        return false;
    }
    for (size_t i = 0; i < k; i += 6) {
        const char* e = norm + i;
        const int p1 = elm327_hex_value(e[0]);
        const int p0 = elm327_hex_value(e[1]);
        const int v1 = elm327_hex_value(e[3]);
        const int v0 = elm327_hex_value(e[4]);
        if (p1 < 0 || p0 < 0 || e[2] != ':' || v1 < 0 || v0 < 0 ||
            (e[5] != 'N' && e[5] != 'F')) {
            return false;
        }
        if (((p1 << 4) | p0) == pp) {
            *out_value = static_cast<uint8_t>((v1 << 4) | v0);
            *out_on = e[5] == 'N';
            return true;
        }
    }
    return false;
}

/* ---- Commands -------------------------------------------------------
 * Every command and request ends with CR. Each writer returns the length
 * written (NUL-terminated), or 0 if it doesn't fit in cap. */

/* A Single Frame on classic CAN carries this much of a message: the
 * service byte, the PID and five data bytes. */
constexpr uint16_t ELM327_SINGLE_FRAME_MAX = 7;

/* The Service 01 PIDs whose reply is taken to fit one Single Frame: bit
 * 0x80 >> (p % 8) of byte p / 8, as the bitmaps carry them. */
struct elm327_one_frame_pids {
    uint8_t bits[32] = {};
};

inline bool elm327_one_frame(const elm327_one_frame_pids& s, uint8_t pid)
{
    return (s.bits[pid / 8] & (0x80u >> (pid % 8))) != 0;
}

inline void elm327_one_frame_set(elm327_one_frame_pids& s, uint8_t pid, bool on)
{
    const uint8_t bit = static_cast<uint8_t>(0x80u >> (pid % 8));
    s.bits[pid / 8] = static_cast<uint8_t>(on ? (s.bits[pid / 8] | bit)
                                              : (s.bits[pid / 8] & ~bit));
}

/* The DIDs whose 22 <DID> reply has needed more than one line: a First
 * Frame and its Consecutive Frames, or a pending reply (7F 22 78) before
 * the answer. Bit 0x80 >> (d % 8) of byte d / 8; empty at attach. */
struct elm327_long_dids {
    uint8_t bits[8192] = {};
};

inline bool elm327_long_did(const elm327_long_dids& s, uint16_t did)
{
    return (s.bits[did / 8] & (0x80u >> (did % 8))) != 0;
}

inline void elm327_long_did_set(elm327_long_dids& s, uint16_t did)
{
    s.bits[did / 8] = static_cast<uint8_t>(s.bits[did / 8] | (0x80u >> (did % 8)));
}

/* Where a def's field ends in its message, or 0 when that isn't known (a
 * text field running to the end of the payload). */
inline uint32_t elm327_def_end(const ecuio_signal_def_t& d)
{
    if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
        return (d.byte_length == 0) ? 0u
                                    : uint32_t{d.start_byte} + d.byte_length;
    }
    return d.start_byte + (uint32_t{d.bit_offset} + d.bit_length + 7) / 8;
}

/* The PIDs a set's requests get the response-count digit for: the
 * supported-PID bitmaps (0x00, 0x20, ... 0xE0: four data bytes), and
 * each PID whose every def keyed 41 <PID> ends within a Single Frame and
 * whose reply, at J1979's length (ECUIO_OBD2_PID_BYTES), fits one too:
 * a def reading the first bytes of a longer reply (PID 68's first
 * sensor of seven bytes) would ask for one frame, and the adapter would
 * cut the reply. For a PID that table doesn't define, the transport
 * takes it out when a reply spans frames, losing that reply. A NULL set
 * gives the bitmaps alone. */
inline void elm327_one_frame_pids_for(const ecuio_signal_set_t* set,
                                      elm327_one_frame_pids* out)
{
    elm327_one_frame_pids listed;
    elm327_one_frame_pids longer;
    if (set != nullptr && set->defs != nullptr) {
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (d.mux_offset != 0 || d.mux_length != 2 ||
                (d.mux_key >> 8) != 0x41u) {
                continue;
            }
            const uint8_t pid = static_cast<uint8_t>(d.mux_key);
            const uint32_t end = elm327_def_end(d);
            elm327_one_frame_set(listed, pid, true);
            const uint32_t j1979 = ECUIO_OBD2_PID_BYTES[pid];
            if (end == 0 || end > ELM327_SINGLE_FRAME_MAX ||
                (j1979 != 0 && 2u + j1979 > ELM327_SINGLE_FRAME_MAX)) {
                elm327_one_frame_set(longer, pid, true);
            }
        }
    }
    for (size_t i = 0; i < sizeof(out->bits); ++i) {
        out->bits[i] = static_cast<uint8_t>(listed.bits[i] & ~longer.bits[i]);
    }
    for (uint32_t pid = 0; pid <= 0xE0; pid += 0x20) {
        elm327_one_frame_set(*out, static_cast<uint8_t>(pid), true);
    }
}

/* The response-count digit for a request (ADR-0026 section 3; the datasheet
 * from v1.3, which counts lines): 1 to a physical ID, so one ECU answers
 * in one line, for a Service 01 request whose PID is in one, for tester
 * present (10 xx has none: an ECU may hold it pending), and for a
 * one-DID 0x22 request whose DID isn't in long_dids (ADR-0020's appendix
 * of 2026-11-01: a scanned DID is mostly refused in one line, and the
 * digit spares the adapter's wait after it); 0, no digit, otherwise. A
 * digit too small ends the reply early: the transport takes a 0x22
 * request back when its reply spanned frames or was held pending (7F 22
 * 78), and asks its DID with no digit from then on. A Service 09 reply
 * can span frames, and a functional ID (0x7DF, 0x18DB33F1) can draw
 * several ECUs. */
inline uint8_t elm327_response_count(const uint8_t* bytes, size_t n,
                                     uint32_t tx_id,
                                     const elm327_one_frame_pids& one,
                                     const elm327_long_dids& long_dids)
{
    if (tx_id == 0x7DFu || tx_id == 0x18DB33F1u) {
        return 0;
    }
    if (n == 2 && bytes[0] == 0x01 && elm327_one_frame(one, bytes[1])) {
        return 1;
    }
    if (n == 2 && bytes[0] == 0x3E && bytes[1] == 0x00) {
        return 1; /* tester present (ADR-0060): 7E 00, never pending */
    }
    if (n == 3 && bytes[0] == 0x22 &&
        !elm327_long_did(long_dids,
                         static_cast<uint16_t>((bytes[1] << 8) | bytes[2]))) {
        return 1;
    }
    return 0;
}

/* The response-count digit for a pack (ADR-0039 Appendix A), whose
 * reply is reply_len bytes: the frames that carries, one Single Frame
 * up to 7 bytes, else a First Frame (6) and Consecutive Frames (7
 * each); 0, no digit, for a functional ID (several ECUs answer), an
 * unknown length or more than 15 frames. */
inline uint8_t elm327_pack_count(uint32_t reply_len, uint32_t tx_id)
{
    if (reply_len == 0 || tx_id == 0x7DFu || tx_id == 0x18DB33F1u) {
        return 0;
    }
    const uint32_t frames =
        reply_len <= 7 ? 1u : 1u + (reply_len - 6u + 6u) / 7u;
    return frames <= 15 ? static_cast<uint8_t>(frames) : 0;
}

inline char elm327_hex_digit(uint32_t v)
{
    return "0123456789ABCDEF"[v & 0x0F];
}

/* "010C1\r": the request's bytes in hex, the response-count digit when
 * count is 1 to 15, then CR. */
inline size_t elm327_format_request(const uint8_t* bytes, size_t n,
                                    uint8_t count, char* out, size_t cap)
{
    const size_t need = 2 * n + (count > 0 ? 1 : 0) + 1;
    if (n == 0 || count > 15 || need + 1 > cap) {
        return 0;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        out[k++] = elm327_hex_digit(bytes[i] >> 4);
        out[k++] = elm327_hex_digit(bytes[i]);
    }
    if (count > 0) {
        out[k++] = elm327_hex_digit(count);
    }
    out[k++] = '\r';
    out[k] = '\0';
    return k;
}

/* "AT<name><id>\r", the ID in three hex digits (11-bit) or eight
 * (29-bit). */
inline size_t elm327_format_id_command(const char* name, uint32_t id,
                                       bool ext29, char* out, size_t cap)
{
    const size_t name_len = strlen(name);
    const size_t digits = ext29 ? 8 : 3;
    const size_t need = 2 + name_len + digits + 1;
    if (need + 1 > cap) {
        return 0;
    }
    size_t k = 0;
    out[k++] = 'A';
    out[k++] = 'T';
    memcpy(out + k, name, name_len);
    k += name_len;
    for (size_t d = digits; d > 0; --d) {
        out[k++] = elm327_hex_digit(id >> (4 * (d - 1)));
    }
    out[k++] = '\r';
    out[k] = '\0';
    return k;
}

/* The transmit ID. 11-bit: "ATSH7E0\r". 29-bit, as the datasheet's
 * v1.4b takes it: the priority (AT CP, the ID's top five bits) and the
 * other three bytes (AT SH xxyyzz), so two commands: "ATCP18\r",
 * "ATSHDAF110\r". Writes them to out[0] (and out[1]); returns how many,
 * 0 if one doesn't fit. */
inline size_t elm327_format_header(uint32_t id, bool ext29, char out[2][24])
{
    if (!ext29) {
        return elm327_format_id_command("SH", id & 0x7FFu, false, out[0], 24)
                   ? 1
                   : 0;
    }
    const uint32_t prio = (id >> 24) & 0x1Fu;
    out[0][0] = 'A';
    out[0][1] = 'T';
    out[0][2] = 'C';
    out[0][3] = 'P';
    out[0][4] = elm327_hex_digit(prio >> 4);
    out[0][5] = elm327_hex_digit(prio);
    out[0][6] = '\r';
    out[0][7] = '\0';
    char* sh = out[1];
    size_t k = 0;
    sh[k++] = 'A';
    sh[k++] = 'T';
    sh[k++] = 'S';
    sh[k++] = 'H';
    for (size_t d = 6; d > 0; --d) {
        sh[k++] = elm327_hex_digit(id >> (4 * (d - 1)));
    }
    sh[k++] = '\r';
    sh[k] = '\0';
    return 2;
}

/* The receive filter for the channels' reply IDs: AT CRA for one, else a
 * filter and mask (AT CF, AT CM) that pass every one of them. The mask
 * keeps the bits all the IDs share, so it may pass others between them;
 * core matches each frame against the channels anyway. Writes one or two
 * commands; returns how many, 0 for no IDs or a command that doesn't
 * fit. */
inline size_t elm327_format_receive_filter(const uint32_t* ids, size_t count,
                                           bool ext29, char out[2][24])
{
    if (count == 0) {
        return 0;
    }
    if (count == 1) {
        return elm327_format_id_command("CRA", ids[0], ext29, out[0], 24) ? 1
                                                                          : 0;
    }
    const uint32_t width = ext29 ? 0x1FFFFFFFu : 0x7FFu;
    uint32_t differ = 0;
    for (size_t i = 1; i < count; ++i) {
        differ |= ids[i] ^ ids[0];
    }
    const uint32_t mask = ~differ & width;
    if (!elm327_format_id_command("CF", ids[0] & mask, ext29, out[0], 24) ||
        !elm327_format_id_command("CM", mask, ext29, out[1], 24)) {
        return 0;
    }
    return 2;
}

/* The protocol for the addressing mode and the CAN rate (ADR-0026 section 2):
 * '6' 11-bit 500 kbit/s, '7' 29-bit 500, '8' 11-bit 250, '9' 29-bit 250;
 * 0 for any other rate. */
inline char elm327_protocol_for(bool ext29, unsigned long baud)
{
    if (baud == 500000) {
        return ext29 ? '7' : '6';
    }
    if (baud == 250000) {
        return ext29 ? '9' : '8';
    }
    return 0;
}

#endif /* ECUIO_ELM327_PROTOCOL_H */
