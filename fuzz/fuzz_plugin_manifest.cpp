// File/layer: fuzz/fuzz_plugin_manifest.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: the plugin loader (ADR-0052, ADR-0066's format 2,
//          ecuio_plugin_load), its pages (ecuio_plugin_page) and its value
//          fixtures (ecuio_plugin_values)
//
// The first byte picks the form: even, a packed package (the rest up to
// its first 0x1E byte is the file); odd, a folder, the rest cut at 0x1E
// bytes into manifest.json and the files the loader asks for in turn.
// What follows (the packed file's 0x1E, or the first piece the folder's
// loader didn't ask for) is a fixture file read against the plugin
// loaded. fuzz/README.md lists what each input must hold to.

#include <ecuio/core.h>
#include <ecuio/dump.h>
#include <ecuio/plugin.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include "json.h"              /* core/src: a page's text read back */
#include "signal_file_build.h" /* core/src: a variant's merged text */
#include "tier2_ceiling.h"     /* core/src: what a conversation may declare */
#include "wasm_check.h"        /* core/src: a module's most bytes */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* The folder's files: each piece after the manifest is the next file
 * the loader asks for, whatever its path */
struct pieces {
    std::vector<std::string> files;
    size_t next = 0;

    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<pieces*>(user);
        check(path != nullptr && path[0] != '\0' && path[0] != '/');
        check(strchr(path, '\\') == nullptr);
        /* No part is "." or "..": the loader checks before it asks */
        const std::string p(path);
        size_t start = 0;
        for (size_t i = 0; i <= p.size(); ++i) {
            if (i == p.size() || p[i] == '/') {
                const std::string part = p.substr(start, i - start);
                check(!part.empty() && part != "." && part != "..");
                start = i + 1;
            }
        }
        if (self->next >= self->files.size()) {
            return 1;
        }
        const std::string& f = self->files[self->next++];
        *bytes = f.data();
        *length = f.size();
        return 0;
    }
};

ecuio_status_t load(const std::string& manifest, pieces* folder,
                    ecuio_plugin_t** out, std::string* words)
{
    if (folder != nullptr) {
        folder->next = 0;
    }
    const ecuio_status_t s = ecuio_plugin_load(
        manifest.data(), manifest.size(),
        folder != nullptr ? &pieces::read : nullptr, folder, out);
    *words = ecuio_last_error_detail();
    return s;
}

/* A fixture file read against both plugins alike: refused in one line
 * with the count untouched, or entries that each name a variant and a
 * numeric field of that variant's set, the first cap copied */
void check_values(const ecuio_plugin_t* a, const ecuio_plugin_t* b,
                  uint32_t variants, const std::string& bytes)
{
    uint32_t count = 0xFFFFFFFFu;
    const ecuio_status_t s =
        ecuio_plugin_values(a, bytes.data(), bytes.size(), nullptr, 0, &count);
    const std::string words = ecuio_last_error_detail();
    uint32_t again = 0xFFFFFFFFu;
    check(ecuio_plugin_values(b, bytes.data(), bytes.size(), nullptr, 0,
                              &again) == s);
    check(words == ecuio_last_error_detail());
    if (s != ECUIO_OK) {
        check(s == ECUIO_ERR_INVALID_ARGUMENT || s == ECUIO_ERR_OUT_OF_MEMORY);
        check(count == 0xFFFFFFFFu && again == 0xFFFFFFFFu);
        check(!words.empty() && words.find('\n') == std::string::npos);
        return;
    }
    check(count == again && count >= 1 && count <= ECUIO_PLUGIN_MAX_VALUES);
    std::vector<ecuio_plugin_value_t> v(count + 1);
    const uint32_t cap = count / 2;
    v[cap].line = 0xABCDu; /* past the cap: left alone */
    uint32_t n = 0;
    check(ecuio_plugin_values(a, bytes.data(), bytes.size(), v.data(), cap,
                              &n) == ECUIO_OK);
    check(n == count && v[cap].line == 0xABCDu);
    check(ecuio_plugin_values(a, bytes.data(), bytes.size(), v.data(), count,
                              &n) == ECUIO_OK);
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_plugin_value_t& e = v[i];
        check(e.variant < variants && e.line >= 1);
        const ecuio_signal_set_t* set = ecuio_plugin_variant_set(a, e.variant);
        check(e.def >= set->defs && e.def < set->defs + set->count);
        check(memchr(e.signal, '\0', sizeof e.signal) != nullptr);
        check(ecuio_plugin_variant_find(a, e.variant, e.signal) == e.def);
        check((e.def->flags & ECUIO_DEF_FLAG_COMPUTED) == 0);
        check(e.def->kind != ECUIO_VALUE_TEXT &&
              e.def->kind != ECUIO_VALUE_HEX);
        for (uint32_t r : e.reserved) {
            check(r == 0);
        }
        for (uint32_t j = 0; j < i; ++j) {
            check(v[j].variant != e.variant || v[j].def != e.def);
        }
    }
}

/* Each declared service once, inside tier 2's ceiling, by sub-function
 * exactly where the ceiling splits; sub_27 gets security access's */
void check_services(const ecuio_plugin_t* a, uint32_t count, bool* sub_27)
{
    ecuio_plugin_service_t sv{};
    sv.struct_size = sizeof sv;
    bool sid_seen[256] = {};
    for (uint32_t i = 0; i < count; ++i) {
        check(ecuio_plugin_service(a, i, &sv) == ECUIO_OK);
        check(sv.struct_size == sizeof sv && sv.flags == 0);
        check(!sid_seen[sv.sid]);
        sid_seen[sv.sid] = true;
        check(sv.sub_count <= ECUIO_PLUGIN_MAX_SUBS);
        check((sv.sub_count > 0) == ecuio_tier2_splits(sv.sid));
        check(sv.sub_count > 0 || ecuio_tier2_allowed(sv.sid, 0));
        for (uint32_t k = 0; k < sizeof sv.subs; ++k) {
            if (k >= sv.sub_count) {
                check(sv.subs[k] == 0);
                continue;
            }
            check(ecuio_tier2_allowed(sv.sid, sv.subs[k]));
            for (uint32_t j = 0; j < k; ++j) {
                check(sv.subs[j] != sv.subs[k]);
            }
            if (sv.sid == 0x27) {
                sub_27[sv.subs[k]] = true;
            }
        }
        check(sv.reserved[0] == 0 && sv.reserved[1] == 0);
    }
    check(ecuio_plugin_service(a, count, &sv) == ECUIO_ERR_INVALID_ARGUMENT);
    if (count > 0) { /* else the index refuses first */
        sv.struct_size = sizeof sv - 1;
        const ecuio_plugin_service_t untouched = sv;
        check(ecuio_plugin_service(a, 0, &sv) == ECUIO_ERR_INVALID_ARGUMENT);
        check(memcmp(&sv, &untouched, sizeof sv) == 0);
    }
}

/* Each security level on one of the group's ECUs, once, inside its
 * bounds, its seed and key requests declared */
void check_levels(const ecuio_plugin_t* a, uint32_t count,
                  const std::vector<uint32_t>& ecus, const bool* sub_27)
{
    ecuio_plugin_security_level_t lv{};
    lv.struct_size = sizeof lv;
    std::vector<ecuio_plugin_security_level_t> levels;
    for (uint32_t i = 0; i < count; ++i) {
        check(ecuio_plugin_security_level(a, i, &lv) == ECUIO_OK);
        check(lv.struct_size == sizeof lv && lv.flags == 0);
        bool listed = false;
        for (const uint32_t known : ecus) {
            listed = listed || known == lv.rx_id;
        }
        check(listed);
        check((lv.level & 1) == 1 && lv.level < 0x7F);
        check(sub_27[lv.level] && sub_27[lv.level + 1]);
        check(lv.seed_bytes >= 1);
        check(lv.key_bytes >= 1 && lv.key_bytes <= ECUIO_PLUGIN_KEY_MAX);
        check(lv.reserved0 == 0);
        check(lv.delay_ms >= 1000 && lv.delay_ms <= 3600000);
        check(lv.key_window_ms >= 50 && lv.key_window_ms <= 4000);
        for (const uint32_t r : lv.reserved) {
            check(r == 0);
        }
        for (const ecuio_plugin_security_level_t& known : levels) {
            check(known.rx_id != lv.rx_id || known.level != lv.level);
        }
        levels.push_back(lv);
    }
    check(ecuio_plugin_security_level(a, count, &lv) ==
          ECUIO_ERR_INVALID_ARGUMENT);
    if (count > 0) { /* else the index refuses first */
        lv.struct_size = sizeof lv - 1;
        const ecuio_plugin_security_level_t untouched = lv;
        check(ecuio_plugin_security_level(a, 0, &lv) ==
              ECUIO_ERR_INVALID_ARGUMENT);
        check(memcmp(&lv, &untouched, sizeof lv) == 0);
    }
}

/* Each ECU the module talks to, once: one of the plugin's, so a channel
 * of every variant's set, with that channel's request ID. Their
 * response IDs, in order */
std::vector<uint32_t> check_ecus(const ecuio_plugin_t* a, uint32_t count,
                                 uint32_t variants)
{
    std::vector<ecuio_plugin_conversation_ecu_t> seen;
    std::vector<uint32_t> ecus;
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_plugin_conversation_ecu_t e{};
        e.struct_size = sizeof e;
        check(ecuio_plugin_conversation_ecu(a, i, &e) == ECUIO_OK);
        check(e.struct_size == sizeof e && e.rx_id != 0 && e.flags == 0);
        const size_t id = strnlen(e.id, sizeof e.id);
        check(id >= 1 && id < sizeof e.id);
        for (const uint32_t r : e.reserved) {
            check(r == 0);
        }
        for (const ecuio_plugin_conversation_ecu_t& known : seen) {
            check(known.rx_id != e.rx_id && strcmp(known.id, e.id) != 0);
        }
        for (uint32_t v = 0; v < variants; ++v) {
            const ecuio_signal_set_t* set =
                ecuio_signal_file_set(ecuio_plugin_variant_file(a, v));
            bool found = false;
            for (uint32_t k = 0; set != nullptr && k < set->channel_count;
                 ++k) {
                found = found || (set->channels[k].rx_id == e.rx_id &&
                                  set->channels[k].tx_id == e.tx_id);
            }
            check(found);
        }
        seen.push_back(e);
        ecus.push_back(e.rx_id);
    }
    ecuio_plugin_conversation_ecu_t past{};
    past.struct_size = sizeof past;
    check(ecuio_plugin_conversation_ecu(a, count, &past) ==
          ECUIO_ERR_INVALID_ARGUMENT);
    return ecus;
}

/* Each page: its form, its text within the bound, the same for both
 * loads, and written again by core to the same bytes */
void check_pages(const ecuio_plugin_t* a, const ecuio_plugin_t* b,
                 uint32_t count)
{
    check(count <= ECUIO_PLUGIN_MAX_PAGES);
    ecuio_plugin_page_t past{};
    past.struct_size = sizeof past;
    check(ecuio_plugin_page(a, count, &past) == ECUIO_ERR_INVALID_ARGUMENT);
    check(ecuio_plugin_page_text(a, count) == nullptr);
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_plugin_page_t page{};
        page.struct_size = sizeof page;
        check(ecuio_plugin_page(a, i, &page) == ECUIO_OK);
        check(page.form == ECUIO_PLUGIN_PAGE_PHONE ||
              page.form == ECUIO_PLUGIN_PAGE_DESKTOP);
        check(page.text_bytes <= ECUIO_PLUGIN_PAGE_MAX_BYTES);
        const char* text = ecuio_plugin_page_text(a, i);
        check(text != nullptr && strlen(text) == page.text_bytes);
        check(std::string(text) == ecuio_plugin_page_text(b, i));
        ecuio_json::Value v;
        ecuio_json::Error error;
        check(ecuio_json::parse(text, page.text_bytes, &v, &error));
        check(ecuio_json::write(v) == text);
    }
}

/* The conversation tier's part, read from both plugins alike: none
 * without the tier; with it, everything inside plugin.h's bounds and
 * tier 2's ceiling, each ECU, service and level once, each level's pair
 * declared, and every getter refusing past its count */
void check_conversation(const ecuio_plugin_t* a, const ecuio_plugin_t* b,
                        uint32_t tiers, uint32_t variants)
{
    ecuio_plugin_conversation_t c{};
    c.struct_size = sizeof c;
    ecuio_plugin_conversation_t d = c;
    const ecuio_status_t s = ecuio_plugin_conversation(a, &c);
    check(ecuio_plugin_conversation(b, &d) == s);
    ecuio_plugin_conversation_ecu_t ecu{};
    ecu.struct_size = sizeof ecu;
    ecuio_plugin_service_t sv{};
    sv.struct_size = sizeof sv;
    ecuio_plugin_security_level_t lv{};
    lv.struct_size = sizeof lv;
    if ((tiers & ECUIO_PLUGIN_TIER_CONVERSATION) == 0) {
        check(s == ECUIO_ERR_INVALID_ARGUMENT);
        check(ecuio_plugin_conversation_ecu(a, 0, &ecu) ==
              ECUIO_ERR_INVALID_ARGUMENT);
        check(ecuio_plugin_service(a, 0, &sv) == ECUIO_ERR_INVALID_ARGUMENT);
        check(ecuio_plugin_security_level(a, 0, &lv) ==
              ECUIO_ERR_INVALID_ARGUMENT);
        return;
    }
    check(s == ECUIO_OK && memcmp(&c, &d, sizeof c) == 0);
    check(c.struct_size == sizeof c && c.reserved[0] == 0 &&
          c.reserved[1] == 0);
    check(c.code_bytes >= 8 && c.code_bytes <= ecuio_wasm::MODULE_MAX_BYTES);
    check(c.budget >= 1 && c.budget <= ECUIO_PLUGIN_BUDGET_MAX);
    check(c.ecu_count >= 1 && c.ecu_count <= ECUIO_MAX_CHANNELS);
    check(c.service_count >= 1 && c.service_count <= ECUIO_PLUGIN_MAX_SERVICES);
    check(c.level_count <= ECUIO_PLUGIN_MAX_LEVELS);

    const std::vector<uint32_t> ecus = check_ecus(a, c.ecu_count, variants);
    bool sub_27[256] = {}; /* security access's sub-functions declared */
    check_services(a, c.service_count, sub_27);
    check_levels(a, c.level_count, ecus, sub_27);
}

bool is_id(const char* s, size_t cap)
{
    const size_t n = strnlen(s, cap);
    if (n < 1 || n >= cap || s[0] == '-' || s[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

ecuio_plugin_operation_step_t step_at(const ecuio_plugin_t* a, uint32_t op,
                                      uint32_t i)
{
    ecuio_plugin_operation_step_t s{};
    s.struct_size = sizeof s;
    check(ecuio_plugin_operation_step(a, op, i, &s) == ECUIO_OK);
    check(s.struct_size == sizeof s && s.flags == 0 && s.reserved0 == 0);
    for (const uint32_t r : s.reserved) {
        check(r == 0);
    }
    check(is_id(s.name, sizeof s.name) && s.dfi == 0);
    return s;
}

/* A kernel read's download and routine, its first two steps */
void check_helper_step(const ecuio_plugin_operation_step_t& s, uint32_t i)
{
    check(s.kind == (i == 0 ? ECUIO_STEP_DOWNLOAD : ECUIO_STEP_ROUTINE));
    check(s.session == ECUIO_STEP_SESSION_PROGRAMMING && s.chunk == 0);
    if (i == 1) {
        check(s.address == 0 && s.bytes == 0 && s.address_bytes == 0 &&
              s.size_bytes == 0 && s.routine_id <= 0xFFFFu);
        return;
    }
    check(s.routine_id == 0 && s.address_bytes >= 1 && s.address_bytes <= 4 &&
          s.size_bytes >= 1 && s.size_bytes <= 4 && s.bytes >= 1);
    check(s.address + s.bytes - 1 <=
          (uint64_t{1} << (8 * s.address_bytes)) - 1);
    check(s.bytes <= (uint64_t{1} << (8 * s.size_bytes)) - 1);
}

/* Returns the ranges' bytes, added; *session their one session */
uint64_t check_steps(const ecuio_plugin_t* a, uint32_t op, uint32_t count,
                     bool kernel, uint32_t* session)
{
    std::vector<ecuio_plugin_operation_step_t> seen;
    uint64_t image = 0;
    const uint32_t lead = kernel ? 2u : 0u;
    check(count > lead);
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_plugin_operation_step_t s = step_at(a, op, i);
        for (const ecuio_plugin_operation_step_t& known : seen) {
            check(strcmp(known.name, s.name) != 0);
        }
        if (i < lead) {
            check_helper_step(s, i);
            seen.push_back(s);
            continue;
        }
        check(s.kind == ECUIO_STEP_READ || s.kind == ECUIO_STEP_UPLOAD);
        check(s.routine_id == 0);
        check(s.session == ECUIO_STEP_SESSION_EXTENDED ||
              s.session == ECUIO_STEP_SESSION_PROGRAMMING);
        check(*session == 0 || *session == s.session);
        *session = s.session;
        check(s.address_bytes >= 1 && s.address_bytes <= 4);
        check(s.size_bytes >= 1 && s.size_bytes <= 4);
        const uint64_t last = (uint64_t{1} << (8 * s.address_bytes)) - 1;
        const uint64_t most = (uint64_t{1} << (8 * s.size_bytes)) - 1;
        check(s.bytes >= 1 && s.bytes <= ECUIO_DUMP_IMAGE_MAX);
        check(s.address + s.bytes - 1 <= last);
        if (s.kind == ECUIO_STEP_READ) {
            check(s.chunk >= 1 && s.chunk <= 4094 && s.chunk <= most);
        } else {
            check(s.chunk == 0 && s.bytes <= most);
        }
        for (size_t k = lead; k < seen.size(); ++k) {
            check(s.address >= seen[k].address + seen[k].bytes ||
                  seen[k].address >= s.address + s.bytes);
        }
        seen.push_back(s);
        image += s.bytes;
    }
    ecuio_plugin_operation_step_t past{};
    past.struct_size = sizeof past;
    check(ecuio_plugin_operation_step(a, op, count, &past) ==
          ECUIO_ERR_INVALID_ARGUMENT);
    return image;
}

void check_kernel(const ecuio_plugin_t* a, const ecuio_plugin_t* b, uint32_t op,
                  uint32_t count, bool kernel)
{
    ecuio_plugin_operation_kernel_t k{};
    k.struct_size = sizeof k;
    ecuio_plugin_operation_kernel_t same = k;
    const uint8_t* blob = nullptr;
    const uint8_t* blob_again = nullptr;
    uint64_t n = 0;
    uint64_t n_again = 0;
    const ecuio_status_t got = ecuio_plugin_operation_kernel(a, op, &k);
    check(ecuio_plugin_operation_kernel(b, op, &same) == got);
    check(ecuio_plugin_operation_blob(a, op, &blob, &n) == got);
    check(ecuio_plugin_operation_blob(b, op, &blob_again, &n_again) == got);
    if (!kernel) {
        check(got == ECUIO_ERR_INVALID_ARGUMENT);
        return;
    }
    check(got == ECUIO_OK && memcmp(&k, &same, sizeof k) == 0);
    check(k.struct_size == sizeof k && k.flags == 0 && k.reserved0 == 0);
    for (const uint32_t r : k.reserved) {
        check(r == 0);
    }
    check(k.ram_bytes >= 1 && k.ram_address + k.ram_bytes - 1 <= 0xFFFFFFFFu);
    check(k.start_count >= 1 && k.start_count <= ECUIO_PLUGIN_MAX_STARTS);
    check(k.quiet_session == 0 || k.quiet_session == 0x82);
    check(k.quiet_keepalive == 0 || k.quiet_keepalive == 0x80);
    check((k.keepalive_ms != 0) == (k.quiet_keepalive != 0) && k.stays <= 1);
    const ecuio_plugin_operation_step_t download = step_at(a, op, 0);
    const ecuio_plugin_operation_step_t routine = step_at(a, op, 1);
    check(blob != nullptr && n == download.bytes && n <= k.ram_bytes &&
          n_again == n && memcmp(blob, blob_again, n) == 0);
    check(download.address >= k.ram_address &&
          download.address - k.ram_address <= k.ram_bytes - n);
    bool listed = false;
    for (uint32_t i = 0; i < k.start_count; ++i) {
        listed = listed || k.start[i] == routine.routine_id;
    }
    check(listed);
    for (uint32_t i = 2; i < count; ++i) {
        const ecuio_plugin_operation_step_t s = step_at(a, op, i);
        check(s.address >= k.ram_address + k.ram_bytes ||
              k.ram_address >= s.address + s.bytes);
    }
}

void check_idents(const ecuio_plugin_t* a, uint32_t op, uint32_t count,
                  uint32_t services)
{
    check(count <= ECUIO_PLUGIN_MAX_IDENTS);
    bool reads = false;
    for (uint32_t i = 0; i < services; ++i) {
        ecuio_plugin_service_t sv{};
        sv.struct_size = sizeof sv;
        check(ecuio_plugin_service(a, i, &sv) == ECUIO_OK);
        reads = reads || sv.sid == 0x22;
    }
    check(count == 0 || reads);
    std::vector<uint32_t> seen;
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_plugin_operation_ident_t d{};
        d.struct_size = sizeof d;
        check(ecuio_plugin_operation_ident(a, op, i, &d) == ECUIO_OK);
        check(d.struct_size == sizeof d && d.flags == 0 && d.did <= 0xFFFFu);
        check(d.role == ECUIO_DUMP_ROLE_HARDWARE ||
              d.role == ECUIO_DUMP_ROLE_SOFTWARE ||
              d.role == ECUIO_DUMP_ROLE_BASE);
        for (const uint32_t r : d.reserved) {
            check(r == 0);
        }
        for (const uint32_t known : seen) {
            check(known != d.did);
        }
        seen.push_back(d.did);
    }
    ecuio_plugin_operation_ident_t past{};
    past.struct_size = sizeof past;
    check(ecuio_plugin_operation_ident(a, op, count, &past) ==
          ECUIO_ERR_INVALID_ARGUMENT);
}

bool unlocks(const ecuio_plugin_t* a, const ecuio_plugin_conversation_t& c,
             const ecuio_plugin_operation_t& op)
{
    bool ecu = false;
    for (uint32_t i = 0; i < c.ecu_count; ++i) {
        ecuio_plugin_conversation_ecu_t e{};
        e.struct_size = sizeof e;
        check(ecuio_plugin_conversation_ecu(a, i, &e) == ECUIO_OK);
        ecu = ecu || (e.rx_id == op.rx_id && e.tx_id == op.tx_id);
    }
    bool level = false;
    for (uint32_t i = 0; i < c.level_count; ++i) {
        ecuio_plugin_security_level_t lv{};
        lv.struct_size = sizeof lv;
        check(ecuio_plugin_security_level(a, i, &lv) == ECUIO_OK);
        level = level || (lv.rx_id == op.rx_id && lv.level == op.level);
    }
    return ecu && level;
}

void check_operations(const ecuio_plugin_t* a, const ecuio_plugin_t* b,
                      uint32_t tiers)
{
    uint32_t count = 0xFFFFFFFFu;
    uint32_t again = 0xFFFFFFFFu;
    check(ecuio_plugin_operation_count(a, &count) == ECUIO_OK);
    check(ecuio_plugin_operation_count(b, &again) == ECUIO_OK);
    check(count == again);
    ecuio_plugin_operation_t past{};
    past.struct_size = sizeof past;
    check(ecuio_plugin_operation(a, count, &past) ==
          ECUIO_ERR_INVALID_ARGUMENT);
    if ((tiers & ECUIO_PLUGIN_TIER_OPERATION) == 0) {
        check(count == 0);
        return;
    }
    check(count >= 1 && count <= ECUIO_PLUGIN_MAX_OPERATIONS);
    ecuio_plugin_conversation_t c{};
    c.struct_size = sizeof c;
    check(ecuio_plugin_conversation(a, &c) == ECUIO_OK);
    std::vector<std::string> ids;
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_plugin_operation_t op{};
        op.struct_size = sizeof op;
        ecuio_plugin_operation_t same = op;
        check(ecuio_plugin_operation(a, i, &op) == ECUIO_OK);
        check(ecuio_plugin_operation(b, i, &same) == ECUIO_OK);
        check(memcmp(&op, &same, sizeof op) == 0);
        check(op.struct_size == sizeof op && op.flags == 0);
        const bool kernel = op.kind == ECUIO_OPERATION_KIND_KERNEL_READ;
        check(op.kind == ECUIO_OPERATION_KIND_DUMP || kernel);
        check(op.op_class == (kernel ? ECUIO_OPERATION_CLASS_WRITE
                                     : ECUIO_OPERATION_CLASS_READ));
        check(unlocks(a, c, op));
        check(op.step_count >= 1 && op.step_count <= ECUIO_PLUGIN_MAX_STEPS);
        check(is_id(op.id, sizeof op.id));
        const size_t title = strnlen(op.title, sizeof op.title);
        check(title >= 1 && title < sizeof op.title);
        for (const uint32_t r : op.reserved) {
            check(r == 0);
        }
        for (const std::string& known : ids) {
            check(known != op.id);
        }
        ids.push_back(op.id);
        uint32_t session = 0;
        check(check_steps(a, i, op.step_count, kernel, &session) ==
              op.image_bytes);
        check(session == ECUIO_STEP_SESSION_PROGRAMMING
                  ? op.leave == ECUIO_OPERATION_LEAVE_DEFAULT ||
                        op.leave == ECUIO_OPERATION_LEAVE_RESET
                  : op.leave == 0);
        check_kernel(a, b, i, op.step_count, kernel);
        check(op.image_bytes <= ECUIO_DUMP_IMAGE_MAX);
        check(op.unlock_sessions == 1);
        check_idents(a, i, op.ident_count, c.service_count);
    }
    past.struct_size = sizeof past - 1;
    const ecuio_plugin_operation_t untouched = past;
    check(ecuio_plugin_operation(a, 0, &past) == ECUIO_ERR_INVALID_ARGUMENT);
    check(memcmp(&past, &untouched, sizeof past) == 0);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return 0;
    }
    const bool folder = (data[0] & 1u) != 0;
    const std::string rest(reinterpret_cast<const char*>(data) + 1, size - 1);
    std::string manifest = rest;
    std::string fixture;
    pieces files;
    if (!folder) {
        const size_t cut = rest.find('\x1E');
        if (cut != std::string::npos) {
            manifest = rest.substr(0, cut);
            fixture = rest.substr(cut + 1);
        }
    } else {
        size_t start = 0;
        bool first = true;
        for (size_t i = 0; i <= rest.size(); ++i) {
            if (i == rest.size() || rest[i] == '\x1E') {
                std::string piece = rest.substr(start, i - start);
                if (first) {
                    manifest = piece;
                    first = false;
                } else {
                    files.files.push_back(piece);
                }
                start = i + 1;
            }
        }
    }

    ecuio_plugin_t* a = nullptr;
    ecuio_plugin_t* b = nullptr;
    std::string wa;
    std::string wb;
    const ecuio_status_t sa =
        load(manifest, folder ? &files : nullptr, &a, &wa);
    const ecuio_status_t sb =
        load(manifest, folder ? &files : nullptr, &b, &wb);
    check(sa == sb);
    check(wa == wb);
    if (sa != ECUIO_OK) {
        check(a == nullptr && b == nullptr);
        check(sa == ECUIO_ERR_INVALID_ARGUMENT ||
              sa == ECUIO_ERR_OUT_OF_MEMORY);
        check(!wa.empty());
        check(wa.find('\n') == std::string::npos);
        check(wa.find('\r') == std::string::npos);
        return 0;
    }
    check(a != nullptr && b != nullptr);
    if (folder && files.next < files.files.size()) {
        fixture = files.files[files.next];
    }

    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    check(ecuio_plugin_info(a, &info) == ECUIO_OK);
    const uint32_t talks =
        ECUIO_PLUGIN_TIER_DATA | ECUIO_PLUGIN_TIER_CONVERSATION;
    check(info.tiers == ECUIO_PLUGIN_TIER_DATA || info.tiers == talks ||
          info.tiers == (talks | ECUIO_PLUGIN_TIER_OPERATION));
    check_conversation(a, b, info.tiers, info.variant_count);
    check_operations(a, b, info.tiers);
    check_pages(a, b, info.page_count);
    check(info.variant_count >= 1 &&
          info.variant_count <= ECUIO_PLUGIN_MAX_VARIANTS);
    check(!folder ? info.fixture_count == 0
                  : info.fixture_count <= ECUIO_PLUGIN_MAX_FIXTURES);

    for (uint32_t v = 0; v < info.variant_count; ++v) {
        const ecuio_signal_set_t* set = ecuio_plugin_variant_set(a, v);
        check(set != nullptr);
        check(ecuio_plugin_variant_set(b, v)->count == set->count);
        ecuio_validation_t why{};
        check(ecuio_validate_signal_set(set, &why) == ECUIO_VALIDATE_OK);
        char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1];
        check(ecuio_plugin_variant(a, v, id, sizeof id) == ECUIO_OK);
        char name[ECUIO_PROFILE_NAME_MAX + 1];
        check(ecuio_plugin_register(a, v, name, sizeof name) == ECUIO_OK);
        const std::string want = std::string("plugin.") + info.id + "." + id;
        check(want == name);
        const ecuio_signal_set_t* found = nullptr;
        check(ecuio_profile_lookup(name, &found) == ECUIO_OK && found == set);
        /* The variant as one signal file: its merged text read back to
         * its own set (an empty one would leave a recording no copy),
         * the same for the same input */
        const ecuio_signal_file_t* file = ecuio_plugin_variant_file(a, v);
        check(file != nullptr && ecuio_signal_file_set(file) == set);
        check(!ecuio_sf::bytes_of(*file).empty());
        check(ecuio_sf::bytes_of(*file) ==
              ecuio_sf::bytes_of(*ecuio_plugin_variant_file(b, v)));
    }

    check_values(a, b, info.variant_count, fixture);

    check(ecuio_plugin_free(a) == ECUIO_OK);
    check(ecuio_plugin_free(b) == ECUIO_OK);
    return 0;
}
