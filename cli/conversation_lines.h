// File/layer: cli/conversation_lines.h | havne-cli, a bound module's
//             conversation and unlock lines (ADR-0066 sections 2.3, 2.4)
// Testing: UNIT (tests/test_cli_unlock.cpp, CliUnlockLineTest); HARNESS
//          CliConsentRunTest.AnUnlockOpensTheGatedLevelOnce
//          (tests/cli_consent.py unlock)
//
// stream's --unlock <ecu>:<level> (the manifest's ECU id, the level in
// hex) and the two lines it brings (schema 1 growth): `conversation`, one
// on each change of a bound channel's report (its generation): the
// module's reply ID, its state, the last word's level and where it
// stands, a refused seed's or key's code, the level's failed attempts and
// its delay left; `unlock`, how core took the one word stream gives.

#ifndef HAVNE_CLI_CONVERSATION_LINES_H
#define HAVNE_CLI_CONVERSATION_LINES_H

#include "jsonl.h"

#include "ecuio/core.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace cli {

// A report's state as the line writes it
inline const char* conversation_state_word(uint8_t state)
{
    switch (state) {
    case ECUIO_CONVERSATION_NONE:
        return "none";
    case ECUIO_CONVERSATION_UNBEGUN:
        return "unbegun";
    case ECUIO_CONVERSATION_TALKING:
        return "talking";
    case ECUIO_CONVERSATION_DECLINED:
        return "declined";
    case ECUIO_CONVERSATION_FAULTED:
        return "faulted";
    case ECUIO_CONVERSATION_ENDED:
        return "ended";
    default:
        return "unknown";
    }
}

// Where the last word stands (ECUIO_UNLOCK_*)
inline const char* unlock_word(uint8_t unlock)
{
    switch (unlock) {
    case ECUIO_UNLOCK_NONE:
        return "none";
    case ECUIO_UNLOCK_WAITING:
        return "waiting";
    case ECUIO_UNLOCK_SEED:
        return "seed";
    case ECUIO_UNLOCK_KEY:
        return "key";
    case ECUIO_UNLOCK_UNLOCKED:
        return "unlocked";
    case ECUIO_UNLOCK_FAILED:
        return "failed";
    case ECUIO_UNLOCK_ENDED:
        return "ended";
    case ECUIO_UNLOCK_LOST:
        return "lost";
    case ECUIO_UNLOCK_NO_LEVEL:
        return "no_level";
    case ECUIO_UNLOCK_KEY_TOO_LONG:
        return "key_too_long";
    case ECUIO_UNLOCK_BUSY:
        return "busy";
    case ECUIO_UNLOCK_DELAYED:
        return "delayed";
    case ECUIO_UNLOCK_ALREADY_UNLOCKED:
        return "already_unlocked";
    case ECUIO_UNLOCK_REFUSED:
        return "refused";
    default:
        return "unknown";
    }
}

// What ended a faulted conversation (ECUIO_CONVERSATION_FAULT_*)
inline const char* conversation_fault_word(uint8_t fault)
{
    switch (fault) {
    case ECUIO_CONVERSATION_FAULT_NONE:
        return "none";
    case ECUIO_CONVERSATION_FAULT_BUDGET:
        return "budget";
    case ECUIO_CONVERSATION_FAULT_TRAP:
        return "trap";
    case ECUIO_CONVERSATION_FAULT_BUFFER:
        return "buffer";
    case ECUIO_CONVERSATION_FAULT_BEGIN:
        return "begin";
    case ECUIO_CONVERSATION_FAULT_LENGTH:
        return "length";
    case ECUIO_CONVERSATION_FAULT_BOUND:
        return "bound";
    case ECUIO_CONVERSATION_FAULT_SERVICE:
        return "service";
    case ECUIO_CONVERSATION_FAULT_UNDECLARED:
        return "undeclared";
    case ECUIO_CONVERSATION_FAULT_DOOR:
        return "door";
    case ECUIO_CONVERSATION_FAULT_KEY:
        return "key";
    case ECUIO_CONVERSATION_FAULT_WINDOW:
        return "window";
    case ECUIO_CONVERSATION_FAULT_CHANNEL:
        return "channel";
    default:
        return "unknown";
    }
}

// "<ecu>:<level>": the ECU's id (1 to 31 bytes, no colon) and the level,
// one or two hex digits above 0; false for any other form
inline bool parse_unlock(const char* text, std::string* ecu, uint8_t* level)
{
    const char* colon = std::strchr(text, ':');
    if (colon == nullptr || colon == text || colon - text > 31) {
        return false;
    }
    const char* hex = colon + 1;
    const size_t digits = std::strlen(hex);
    if (digits < 1 || digits > 2) {
        return false;
    }
    unsigned v = 0;
    for (size_t i = 0; i < digits; ++i) {
        const char c = hex[i];
        const int d = (c >= '0' && c <= '9')   ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                               : -1;
        if (d < 0) {
            return false;
        }
        v = v * 16 + static_cast<unsigned>(d);
    }
    if (v == 0) {
        return false;
    }
    ecu->assign(text, static_cast<size_t>(colon - text));
    *level = static_cast<uint8_t>(v);
    return true;
}

// A byte as frames.csv's two digits ("01"), or null for 0
inline void byte_or_null(Line& l, uint8_t b)
{
    if (b == 0) {
        l.raw("null");
    } else {
        l.hex_bytes(&b, 1);
    }
}

// The conversation line for one channel's report
inline void conversation_line(Line& l, const ecuio_channel_conversation_t& r)
{
    l.raw("{\"type\":\"conversation\"");
    l.key("ecu");
    l.hex_id(r.rx_id);
    l.key("state");
    l.string(conversation_state_word(r.state), SIZE_MAX);
    if (r.fault != ECUIO_CONVERSATION_FAULT_NONE) {
        l.key("fault");
        l.string(conversation_fault_word(r.fault), SIZE_MAX);
    }
    l.key("level");
    byte_or_null(l, r.level);
    l.key("unlock");
    l.string(unlock_word(r.unlock), SIZE_MAX);
    l.key("nrc");
    byte_or_null(l, r.nrc);
    l.key("failed");
    l.u64(r.failed);
    l.key("delay_left_ms");
    l.u64(r.delay_left_ms);
    l.key("kept");
    l.raw((r.flags & ECUIO_CONVERSATION_NOT_KEPT) ? "false" : "true");
    l.ch('}');
}

// How core took stream's word: "asked" (the attempt is under way: the
// conversation lines say how it ends), "delayed" (the level's delay runs,
// wait_ms left: nothing sent) or "refused" (nothing sent), with core's
// words for the last two
inline void unlock_line(Line& l, uint32_t rx, uint8_t level,
                        ecuio_status_t status, uint32_t wait_ms,
                        const char* detail)
{
    l.raw("{\"type\":\"unlock\"");
    l.key("ecu");
    l.hex_id(rx);
    l.key("level");
    byte_or_null(l, level);
    l.key("outcome");
    l.string(status == ECUIO_OK             ? "asked"
             : status == ECUIO_ERR_TOO_SOON ? "delayed"
                                            : "refused",
             SIZE_MAX);
    if (status == ECUIO_ERR_TOO_SOON) {
        l.key("wait_ms");
        l.u64(wait_ms);
    }
    if (status != ECUIO_OK) {
        l.key("code");
        l.u64(static_cast<uint32_t>(status));
        l.key("detail");
        l.string(detail != nullptr ? detail : "", SIZE_MAX);
    }
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_CONVERSATION_LINES_H
