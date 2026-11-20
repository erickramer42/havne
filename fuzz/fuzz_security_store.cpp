// File/layer: fuzz/fuzz_security_store.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_security_store

#include "security_store.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <string>

namespace {

using ecuio_security::kStoreMaxBytes;
using ecuio_security::kStoreMaxEntries;
using ecuio_security::kStoreMaxFailed;
using ecuio_security::kStoreMaxUtc;
using ecuio_security::store;
using ecuio_security::store_entry;
using ecuio_security::store_io;

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* What a store that read holds, by the harness's own reading of the
 * rules: each entry inside its bounds with something to keep and no mark
 * left, each (vehicle, rx, level) once */
void check_store(const store& s)
{
    check(s.entries.size() <= kStoreMaxEntries);
    for (size_t k = 0; k < s.entries.size(); ++k) {
        const store_entry& e = s.entries[k];
        check(e.rx >= 1 && e.rx <= 0x1FFFFFFF);
        check((e.level & 1) != 0 && e.level <= 0x7D && e.level != 0x5F);
        check(e.failed <= kStoreMaxFailed);
        check(e.last_utc >= 0 && e.last_utc <= kStoreMaxUtc);
        check(e.pending_utc == 0);
        check(e.failed != 0 || e.last_utc != 0);
        for (size_t j = 0; j < k; ++j) {
            check(s.entries[j].vehicle != e.vehicle ||
                  s.entries[j].rx != e.rx || s.entries[j].level != e.level);
        }
    }
}

} // namespace

/* Arbitrary bytes through the store's reader; what reads is written and
 * read again to the same entries */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    store s;
    std::string why;
    const store_io io = ecuio_security::store_read(
        reinterpret_cast<const char*>(data), size, &s, &why);
    if (io != store_io::ok) {
        check(s.entries.empty());
        check(io == store_io::out_of_memory || !why.empty());
        check(why.find('\n') == std::string::npos);
        return 0;
    }
    check(size <= kStoreMaxBytes);
    check_store(s);
    const std::string text = ecuio_security::store_text(s);
    check(text.size() <= kStoreMaxBytes);
    store back;
    check(ecuio_security::store_read(text.data(), text.size(), &back, &why) ==
          store_io::ok);
    check(back.entries.size() == s.entries.size());
    for (size_t k = 0; k < s.entries.size(); ++k) {
        const store_entry& a = s.entries[k];
        const store_entry& b = back.entries[k];
        check(a.vehicle == b.vehicle && a.rx == b.rx && a.level == b.level &&
              a.failed == b.failed && a.last_utc == b.last_utc &&
              b.pending_utc == 0);
    }
    return 0;
}
