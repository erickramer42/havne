// File/layer: core/src/consent_lines.cpp | C ABI, C++17, Qt-free, every
//             platform
// Testing: UNIT: ConsentLinesTest (test_core); test_c_abi
//
// The lines a consent shows (ADR-0066's appendix of 2026-11-23): Havne's
// own, the same for every plugin, then the lines of the classes the
// plugin's code falls in, in one table so every front end shows the same
// words. The words are the consent screen's and gate 3's, word for word.
// The write class's lines land with the write path; their ids are taken
// in plugin.h so the order holds.

#include "ecuio/plugin.h"

#include <cstddef>
#include <cstdint>

namespace {

constexpr uint32_t kConv = ECUIO_CONSENT_SET_CONVERSATION;
constexpr uint32_t kRead = ECUIO_CONSENT_SET_READ;
constexpr uint32_t kAction = ECUIO_CONSENT_LINE_ACTION;

struct line_row {
    uint32_t id;
    uint32_t sets; /* the ECUIO_CONSENT_SET_* that show it; 0 none yet */
    uint32_t flags;
    const char* key;
    const char* heading;
    const char* text;
};

/* In the order a consent shows them */
constexpr line_row kLines[] = {
    {ECUIO_CONSENT_LINE_HAVNE_1, kConv | kRead, 0, "asks", "",
     "{name} {version} asks to send requests to your car's modules, beyond "
     "the reads Havne makes on its own."},
    {ECUIO_CONSENT_LINE_HAVNE_2, kConv | kRead, 0, "checked", "",
     "Havne checks each request against the services the plugin declares "
     "and against its own limits."},
    {ECUIO_CONSENT_LINE_WRITE_ON_ASK, 0, 0, "write-on-ask", "",
     "A plugin can't send a write on its own: writing, if the plugin offers "
     "it, asks again at the time."},
    {ECUIO_CONSENT_LINE_HAVNE_3, kConv | kRead, 0, "can-lock", "",
     "Some requests can lock a module for a while or for good if they go "
     "wrong. Havne can't tell whether this plugin's content is correct."},
    {ECUIO_CONSENT_LINE_HAVNE_4, kConv | kRead, 0, "publisher-answers", "",
     "The publisher below, not the Havne project, answers for what this "
     "plugin contains and what it causes to be sent."},
    {ECUIO_CONSENT_LINE_HAVNE_5, kConv | kRead, 0, "this-version", "",
     "Allowing is for this version only. If the plugin changes in any way, "
     "Havne asks again."},
    {ECUIO_CONSENT_LINE_NOT_A_CONFIRMATION, kConv | kRead, 0,
     "not-a-confirmation", "",
     "Consent lets this plugin bind to a session and run its operations. It "
     "is not a confirmation of any write: each write asks again."},
    {ECUIO_CONSENT_LINE_EMISSIONS_READ, kRead, 0, "emissions-read",
     "Emissions law: reading changes nothing.",
     "Reading a calibration changes nothing in the vehicle. Changing it "
     "later may be restricted by emissions law, and this copy is the way "
     "back to the original."},
    {ECUIO_CONSENT_LINE_WARRANTY_READ, kRead, 0, "warranty-read",
     "A read may leave a record in the module.",
     "Some modules record a diagnostic session or a programming-session "
     "entry. A read can leave such a record."},
    {ECUIO_CONSENT_LINE_ENGINE_STATE, kConv | kRead, kAction, "engine-state",
     "",
     "A diagnostic session, and above all the programming session, can stop "
     "the module's normal work: an engine or transmission module may stop "
     "controlling the vehicle until the session ends or the vehicle is "
     "reset. Do this only with the vehicle parked, and never while it is "
     "moving."},
    {ECUIO_CONSENT_LINE_LOCKOUT, kConv | kRead, kAction, "lockout", "",
     "The unlock sends a key a plugin computed. A wrong key counts toward a "
     "lockout, and some modules lock for a long time, or for good, after a "
     "few wrong keys. Havne never retries an unlock by itself."},
    {ECUIO_CONSENT_LINE_LOCKOUT_COUNT, 0, kAction, "lockout-count", "",
     "This run will make [K] unlock attempts, one for each session it reads "
     "or writes in. This module has [N] failed attempts on record."},
    {ECUIO_CONSENT_LINE_COPY_DATA, kRead, 0, "copy-data",
     "The copy holds identifying data.",
     "The copy holds the module's identifiers and may hold the vehicle's "
     "identity or security data (a VIN, immobilizer data) that no scrub "
     "removes. Havne never deletes it without asking, but it can still be "
     "lost with the phone, an uninstall or a disk. On Android it is not in "
     "the phone's backup, so export it from Havne to keep it. A shared copy "
     "can identify the vehicle and the owner."},
    {ECUIO_CONSENT_LINE_COPYRIGHT, kRead, 0, "copyright",
     "The calibration is someone's software.",
     "A calibration is software the manufacturer or a supplier wrote, and "
     "its license or the law may limit copying, changing or sharing it. What "
     "you do with a copy is your responsibility."},
};

constexpr uint32_t kSets = kConv | kRead;

} // namespace

extern "C" {

ecuio_status_t ecuio_consent_line(uint32_t set, uint32_t index,
                                  ecuio_consent_line_t* out)
{
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (set == 0 || (set & ~kSets) != 0 ||
        out->struct_size < sizeof(ecuio_consent_line_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    uint32_t seen = 0;
    for (const line_row& row : kLines) {
        if ((row.sets & set) == 0) {
            continue;
        }
        if (seen++ == index) {
            ecuio_consent_line_t l{};
            l.struct_size = sizeof l;
            l.id = row.id;
            l.flags = row.flags;
            l.key = row.key;
            l.text = row.text;
            l.heading = row.heading;
            *out = l;
            return ECUIO_OK;
        }
    }
    return ECUIO_ERR_INVALID_ARGUMENT;
}

} // extern "C"
