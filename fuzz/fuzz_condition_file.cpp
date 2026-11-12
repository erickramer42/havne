// File/layer: fuzz/fuzz_condition_file.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_condition_file

#include <ecuio/conditions.h>
#include <ecuio/core.h>
#include <ecuio/profile.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <string>

namespace {

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* A buffer holds text terminated inside it, zero after the NUL */
bool text_in(const char* buf, size_t cap)
{
    const void* nul = memchr(buf, 0, cap);
    if (nul == nullptr) {
        return false;
    }
    for (const char* p = static_cast<const char*>(nul); p < buf + cap; ++p) {
        if (*p != 0) {
            return false;
        }
    }
    return true;
}

bool key_text(const char* k, size_t max)
{
    const size_t n = strlen(k);
    if (n == 0 || n > max) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

/* What any term the loader keeps must be */
void check_term(const ecuio_condition_term_t& t)
{
    check(t.reserved0 == 0 && t.reserved[0] == 0 && t.reserved[1] == 0);
    check(text_in(t.unit, sizeof t.unit));
    check(std::isfinite(t.level) && std::isfinite(t.clear));
    for (uint32_t k = t.reference_count; k < ECUIO_CONDITION_MAX_REFERENCES;
         ++k) {
        const ecuio_condition_reference_t zero{};
        check(memcmp(&t.references[k], &zero, sizeof zero) == 0);
    }
    if (t.kind != ECUIO_TERM_SIGNAL) {
        check(t.kind == ECUIO_TERM_LINK || t.kind == ECUIO_TERM_CHECK);
        check(t.kind == ECUIO_TERM_LINK
                  ? t.test >= ECUIO_TERM_LINK_NOT_OK &&
                        t.test <= ECUIO_TERM_LINK_HARD_FAILURE
                  : t.test >= ECUIO_CHECK_GAP && t.test <= ECUIO_CHECK_ISOTP);
        check(t.level == 0.0 && t.clear == 0.0 && t.max_age_us == 0 &&
              t.flags == 0 && t.reference_count == 0 && t.unit[0] == '\0');
        return;
    }
    check(t.test >= ECUIO_TERM_ABOVE && t.test <= ECUIO_TERM_DIFFERS);
    check((t.flags & ~(ECUIO_TERM_HAS_CLEAR | ECUIO_TERM_HAS_UNIT)) == 0);
    const bool ordered =
        t.test == ECUIO_TERM_ABOVE || t.test == ECUIO_TERM_BELOW;
    const bool has_unit = (t.flags & ECUIO_TERM_HAS_UNIT) != 0;
    check(has_unit || (!ordered && t.unit[0] == '\0'));
    if ((t.flags & ECUIO_TERM_HAS_CLEAR) != 0) {
        check(ordered);
        check(t.test == ECUIO_TERM_ABOVE ? t.clear <= t.level
                                         : t.clear >= t.level);
    } else {
        check(t.clear == 0.0);
    }
    check(t.max_age_us == 0 || (t.max_age_us >= ECUIO_COMPUTED_MIN_AGE_US &&
                                t.max_age_us <= ECUIO_COMPUTED_MAX_AGE_US));
    check(t.reference_count >= 1 &&
          t.reference_count <= ECUIO_CONDITION_MAX_REFERENCES);
    for (uint32_t k = 0; k < t.reference_count; ++k) {
        const ecuio_condition_reference_t& r = t.references[k];
        check(text_in(r.text, sizeof r.text) && r.text[0] != '\0');
        if (r.kind == ECUIO_REFERENCE_KEY) {
            check(r.id == 0 && strncmp(r.text, "key:", 4) == 0 &&
                  key_text(r.text + 4, 31));
        } else {
            check(r.kind == ECUIO_REFERENCE_ID && r.id <= UINT16_MAX);
            check(strncmp(r.text, "id:", 3) == 0 ||
                  strncmp(r.text, "obd2.", 5) == 0);
        }
        for (uint32_t j = 0; j < k; ++j) {
            const ecuio_condition_reference_t& o = t.references[j];
            check(o.kind != r.kind ||
                  (r.kind == ECUIO_REFERENCE_ID ? o.id != r.id
                                                : strcmp(o.text, r.text) != 0));
        }
    }
}

/* How a condition binds to a set: every answer within the contract, and
 * a term that names a def names one the set holds */
void check_binding(const ecuio_condition_set_t* set, uint32_t i,
                   const ecuio_condition_info_t& info,
                   const ecuio_signal_set_t* signals,
                   const ecuio_signal_file_t* file)
{
    ecuio_condition_binding_t b;
    memset(&b, 0xA5, sizeof b);
    check(ecuio_condition_set_resolve(set, i, signals, file, &b) == ECUIO_OK);
    check(b.term_count == info.term_count && b.reserved0 == 0 &&
          b.reserved[0] == 0 && b.reserved[1] == 0);
    check(b.why <= ECUIO_CONDITION_WHY_ENUM_ORDER);
    const bool disabled = (info.flags & ECUIO_CONDITION_INFO_DISABLED) != 0;
    check(disabled == (b.why == ECUIO_CONDITION_WHY_DISABLED));
    check((b.why == ECUIO_CONDITION_WHY_NONE || disabled)
              ? b.why_term == UINT32_MAX
              : b.why_term < info.term_count);
    for (uint32_t k = 0; k < ECUIO_CONDITION_MAX_TERMS; ++k) {
        check((b.reference[k] == UINT32_MAX) == (b.signal_id[k] == UINT32_MAX));
        if (b.signal_id[k] == UINT32_MAX) {
            continue;
        }
        check(k < info.term_count && signals != nullptr);
        ecuio_condition_term_t t;
        check(ecuio_condition_set_get_term(set, i, k, &t) == ECUIO_OK);
        check(t.kind == ECUIO_TERM_SIGNAL &&
              b.reference[k] < t.reference_count);
        /* The reference taken names the def, and none before it names
         * one */
        const auto with_id = [&](uint32_t id) {
            const ecuio_signal_def_t* found = nullptr;
            for (uint32_t d = 0; d < signals->count; ++d) {
                if (signals->defs[d].id == id) {
                    found = &signals->defs[d];
                }
            }
            return found;
        };
        const auto named = [&](const ecuio_condition_reference_t& r) {
            if (r.kind == ECUIO_REFERENCE_KEY) {
                return ecuio_signal_file_find(file, r.text + 4);
            }
            return r.id < ECUIO_UDS_GENERIC_SIG_ID_BASE ? with_id(r.id)
                                                        : nullptr;
        };
        const ecuio_signal_def_t* def = with_id(b.signal_id[k]);
        check(def != nullptr);
        check(named(t.references[b.reference[k]]) == def);
        for (uint32_t j = 0; j < b.reference[k]; ++j) {
            check(named(t.references[j]) == nullptr);
        }
        if (b.why == ECUIO_CONDITION_WHY_NONE) {
            check((def->flags & ECUIO_DEF_FLAG_COMPUTED) != 0 ||
                  (def->kind != ECUIO_VALUE_TEXT &&
                   def->kind != ECUIO_VALUE_HEX));
            check(strncmp(t.unit, def->unit, sizeof def->unit) == 0);
        }
    }
}

} // namespace

/* The input is a condition file, or one and then, after a 0x1E byte, a
 * signal file to resolve it against */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const char* bytes = reinterpret_cast<const char*>(data);
    const void* cut = memchr(data, 0x1E, size);
    const size_t length =
        cut != nullptr
            ? static_cast<size_t>(static_cast<const uint8_t*>(cut) - data)
            : size;

    ecuio_condition_set_t* a = nullptr;
    const ecuio_status_t sa = ecuio_condition_set_load(bytes, length, &a);
    const std::string detail_a = ecuio_last_error_detail();
    ecuio_condition_set_t* b = nullptr;
    const ecuio_status_t sb = ecuio_condition_set_load(bytes, length, &b);
    check(sa == sb);
    check(detail_a == ecuio_last_error_detail());

    if (sa != ECUIO_OK) {
        check(a == nullptr && b == nullptr);
        check(sa == ECUIO_ERR_INVALID_ARGUMENT ||
              sa == ECUIO_ERR_OUT_OF_MEMORY);
        check(!detail_a.empty());
        check(detail_a.find('\n') == std::string::npos);
        check(detail_a.find('\r') == std::string::npos);
        return 0;
    }
    check(a != nullptr && b != nullptr);
    check(length <= ECUIO_CONDITION_FILE_MAX_BYTES);

    ecuio_signal_file_t* file = nullptr;
    if (cut != nullptr) {
        (void)ecuio_signal_file_load(bytes + length + 1, size - length - 1,
                                     &file);
    }

    const uint32_t count = ecuio_condition_set_count(a);
    check(count <= ECUIO_CONDITION_MAX_CONDITIONS &&
          count == ecuio_condition_set_count(b));
    check(strlen(ecuio_condition_set_name(a)) <= 63);
    check(strcmp(ecuio_condition_set_name(a), ecuio_condition_set_name(b)) ==
          0);

    ecuio_check_options_t checks;
    memset(&checks, 0xA5, sizeof checks);
    check(ecuio_condition_set_checks(a, &checks) == ECUIO_OK);
    check(checks.gap_us <= ECUIO_CONDITION_MAX_CHECK_US &&
          checks.settle_us <= ECUIO_CONDITION_MAX_CHECK_US &&
          checks.flags == 0);
    check(checks.rate_drop_x100 == 0 ||
          (checks.rate_drop_x100 > 100 &&
           checks.rate_drop_x100 <= ECUIO_CONDITION_MAX_RATE_DROP_X100));
    for (uint64_t r : checks.reserved) {
        check(r == 0);
    }

    for (uint32_t i = 0; i < count; ++i) {
        ecuio_condition_info_t info;
        ecuio_condition_info_t other;
        memset(&info, 0xA5, sizeof info);
        check(ecuio_condition_set_get(a, i, &info) == ECUIO_OK);
        check(ecuio_condition_set_get(b, i, &other) == ECUIO_OK);
        check(key_text(info.key, ECUIO_CONDITION_KEY_MAX));
        check(strcmp(info.key, other.key) == 0 &&
              strcmp(info.name, other.name) == 0);
        const size_t name = strlen(info.name);
        check(name >= 1 && name <= ECUIO_CONDITION_NAME_MAX);
        check(strchr("=+-@\t\r", info.name[0]) == nullptr);
        for (uint32_t j = 0; j < i; ++j) {
            check(ecuio_condition_set_get(a, j, &other) == ECUIO_OK);
            check(strcmp(info.key, other.key) != 0 &&
                  strcmp(info.name, other.name) != 0);
        }
        check((info.flags & ~(ECUIO_CONDITION_INFO_DISABLED |
                              ECUIO_CONDITION_INFO_ANY)) == 0);
        const uint32_t alert =
            ECUIO_CONDITION_ALERT_SOUND | ECUIO_CONDITION_ALERT_BANNER;
        check((info.uses &
               ~(ECUIO_CONDITION_USE_ALERT | ECUIO_CONDITION_USE_RECORD |
                 ECUIO_CONDITION_USE_MARK | alert)) == 0);
        const bool alerts = (info.uses & ECUIO_CONDITION_USE_ALERT) != 0;
        check(alerts ? (info.alert_level == ECUIO_ALERT_WARNING ||
                        info.alert_level == ECUIO_ALERT_DANGER)
                     : (info.alert_level == ECUIO_ALERT_NONE &&
                        (info.uses & alert) == 0));
        check(info.term_count >= 1 &&
              info.term_count <= ECUIO_CONDITION_MAX_TERMS);
        check(info.for_us <= ECUIO_CONDITION_MAX_FOR_US &&
              info.clear_for_us <= ECUIO_CONDITION_MAX_FOR_US &&
              info.before_us <= ECUIO_CONDITION_MAX_BEFORE_US &&
              info.after_us <= ECUIO_CONDITION_MAX_AFTER_US);
        check((info.uses & ECUIO_CONDITION_USE_RECORD) != 0 ||
              (info.before_us == 0 && info.after_us == 0));
        check(info.reserved[0] == 0 && info.reserved[1] == 0);

        ecuio_condition_term_t t;
        for (uint32_t k = 0; k < info.term_count; ++k) {
            ecuio_condition_term_t u;
            memset(&t, 0xA5, sizeof t);
            check(ecuio_condition_set_get_term(a, i, k, &t) == ECUIO_OK);
            check(ecuio_condition_set_get_term(b, i, k, &u) == ECUIO_OK);
            check(memcmp(&t, &u, sizeof t) == 0);
            check_term(t);
        }
        check(ecuio_condition_set_get_term(a, i, info.term_count, &t) ==
              ECUIO_ERR_INVALID_ARGUMENT);

        check_binding(a, i, info, nullptr, nullptr);
        check_binding(a, i, info, ecuio_obd2_signal_set(), nullptr);
        check_binding(a, i, info, ecuio_mock_signal_set(), nullptr);
        if (file != nullptr) {
            check_binding(a, i, info, ecuio_signal_file_set(file), file);
            check_binding(a, i, info, ecuio_signal_file_set(file), nullptr);
        }
    }
    ecuio_condition_info_t past;
    check(ecuio_condition_set_get(a, count, &past) ==
          ECUIO_ERR_INVALID_ARGUMENT);

    ecuio_signal_file_free(file);
    ecuio_condition_set_free(a);
    ecuio_condition_set_free(b);
    return 0;
}
