// File/layer: fuzz/fuzz_obdb_import.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_obdb_import

#include <ecuio/core.h>
#include <ecuio/obdb.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace {

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

bool one_line(const char* s)
{
    return s != nullptr && strchr(s, '\n') == nullptr &&
           strchr(s, '\r') == nullptr;
}

struct Seen {
    uint32_t converted = 0;
    std::string all; /* every note, so two imports compare */
};

int note(const ecuio_obdb_note_t* n, void* user)
{
    auto* seen = static_cast<Seen*>(user);
    check(n->what <= ECUIO_OBDB_NOTE_FILE);
    check(one_line(n->signal) && one_line(n->key) && one_line(n->field) &&
          one_line(n->text));
    check(n->text[0] != '\0');
    check((n->what == ECUIO_OBDB_NOTE_SKIPPED) == (n->key[0] == '\0') ||
          n->what == ECUIO_OBDB_NOTE_FILE);
    seen->converted += n->what == ECUIO_OBDB_NOTE_CONVERTED ? 1u : 0u;
    seen->all += std::to_string(n->what) + ' ' + std::to_string(n->line) + ' ' +
                 n->signal + ' ' + n->key + ' ' + n->field + ' ' + n->text +
                 '\n';
    return 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return 0;
    }
    ecuio_obdb_options_t options{};
    options.model_year =
        (data[0] & 0x7Fu) == 0 ? 0u : 1990u + (data[0] & 0x3Fu);
    options.flags = (data[0] & 0x80u) != 0 ? ECUIO_OBDB_ALONE : 0u;
    const char* bytes = reinterpret_cast<const char*>(data + 1);
    const size_t length = size - 1;

    Seen a;
    char* json_a = nullptr;
    size_t length_a = 0;
    const ecuio_status_t sa = ecuio_obdb_import(bytes, length, &options, note,
                                                &a, &json_a, &length_a);
    const std::string detail_a = ecuio_last_error_detail();
    Seen b;
    char* json_b = nullptr;
    size_t length_b = 0;
    const ecuio_status_t sb = ecuio_obdb_import(bytes, length, &options, note,
                                                &b, &json_b, &length_b);
    check(sa == sb);
    check(detail_a == ecuio_last_error_detail());
    check(a.all == b.all);

    if (sa != ECUIO_OK) {
        check(json_a == nullptr && json_b == nullptr);
        check(length_a == 0 && length_b == 0);
        check(sa == ECUIO_ERR_INVALID_ARGUMENT ||
              sa == ECUIO_ERR_OUT_OF_MEMORY);
        check(!detail_a.empty() && one_line(detail_a.c_str()));
        /* The output always loads: a refusal of its own is a bug */
        check(detail_a.find("the converted file is refused") != 0);
        return 0;
    }
    check(json_a != nullptr && json_b != nullptr);
    check(length_a == length_b && memcmp(json_a, json_b, length_a) == 0);
    check(strlen(json_a) == length_a);

    ecuio_signal_file_t* file = nullptr;
    check(ecuio_signal_file_load(json_a, length_a, &file) == ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    uint32_t own = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        own += set->defs[i].id >= ECUIO_USER_SIG_ID_BASE ? 1u : 0u;
    }
    check(own == a.converted);
    check(own > 0);
    check((options.flags & ECUIO_OBDB_ALONE) == 0 || own == set->count);
    ecuio_signal_file_free(file);
    ecuio_obdb_free(json_a);
    ecuio_obdb_free(json_b);
    return 0;
}
