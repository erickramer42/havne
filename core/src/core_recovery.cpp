#include "ecuio/operation.h"
#include "recovery_open.h"

#include "conversation_registry.h"
#include "error_detail.h"
#include "session.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

namespace {

constexpr char kNeededWords[] =
    "a flash of this module didn't finish: recover it before it runs";
constexpr char kNothingErasedWords[] =
    "the flash stopped before anything was erased; the module is likely "
    "intact: recovering it is still offered";

uint32_t plan_index(const ecuio_session* s, const std::string& id, uint32_t rx)
{
    const ecuio_conversation_part* part = s->conversation.get();
    for (uint32_t i = 0; part != nullptr && i < part->operations.size(); ++i) {
        if (part->operations[i].id == id && part->operations[i].rx_id == rx) {
            return i;
        }
    }
    return UINT32_MAX;
}

void unreadable(ecuio_session* s, uint32_t c, const std::string& why)
{
    const std::lock_guard<std::mutex> lock(s->support_mutex);
    ecuio_module_recovery_t& r = s->recovery[c];
    r = ecuio_module_recovery_t{};
    r.state = ECUIO_RECOVERY_UNREADABLE;
    r.operation = UINT32_MAX;
    std::snprintf(r.words, sizeof r.words, "%s", why.c_str());
}

} // namespace

void ecuio_recovery_needed(ecuio_session* s, uint32_t c,
                           const ecuio_operation::journal_record& j) noexcept
{
    ecuio_module_recovery_t& r = s->recovery[c];
    r = ecuio_module_recovery_t{};
    r.state = ECUIO_RECOVERY_NEEDED;
    r.attempts = j.head.recoveries;
    r.operation = plan_index(s, j.head.operation, j.head.rx);
    r.sectors = static_cast<uint16_t>(j.head.sectors.size());
    r.sectors_written = static_cast<uint16_t>(j.mark.written);
    const bool nothing = j.mark.erased == 0;
    r.flags = (j.head.copy.empty() ? 0u : ECUIO_RECOVERY_COPY_PINNED) |
              (nothing ? ECUIO_RECOVERY_NOTHING_ERASED : 0u);
    std::memcpy(r.words, nothing ? kNothingErasedWords : kNeededWords,
                nothing ? sizeof kNothingErasedWords : sizeof kNeededWords);
}

void ecuio_recovery_open(ecuio_session* s) noexcept
{
    if (s->journal_folder.empty()) {
        return;
    }
    const size_t n = s->module_rx.size() < ECUIO_MAX_CHANNELS
                         ? s->module_rx.size()
                         : size_t{ECUIO_MAX_CHANNELS};
    for (uint32_t c = 0; c < n; ++c) {
        try {
            ecuio_operation::journal_record j;
            std::string why;
            const ecuio_operation::journal_found found =
                ecuio_operation::journal_load(s->journal_folder,
                                              s->module_rx[c], &j, &why);
            if (found == ecuio_operation::journal_found::none) {
                continue;
            }
            if (found == ecuio_operation::journal_found::failed) {
                unreadable(s, c, why);
            } else {
                const std::lock_guard<std::mutex> lock(s->support_mutex);
                ecuio_recovery_needed(s, c, j);
            }
        } catch (const std::bad_alloc&) {
            unreadable(s, c,
                       "the flash journal couldn't be read: out of memory");
        }
        s->part_written.fetch_or(1u << c);
    }
}

extern "C" {

ecuio_status_t ecuio_session_journal_folder(ecuio_session_t* session,
                                            const char* folder_utf8)
{
    ecuio_detail_clear();
    if (session == nullptr || folder_utf8 == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->started) {
        ecuio_detail_set("the journal folder is named before start");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    try {
        session->journal_folder = folder_utf8;
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_module_recovery(const ecuio_session_t* session,
                                                 uint32_t channel,
                                                 ecuio_module_recovery_t* out)
{
    if (session == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out->struct_size < sizeof *out || channel >= ECUIO_MAX_CHANNELS ||
        channel >= session->module_rx.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    {
        const std::lock_guard<std::mutex> lock(session->support_mutex);
        *out = session->recovery[channel];
    }
    out->struct_size = sizeof *out;
    return ECUIO_OK;
}

} // extern "C"
