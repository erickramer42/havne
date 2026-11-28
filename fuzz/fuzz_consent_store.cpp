// File/layer: fuzz/fuzz_consent_store.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_consent_store

#include "consent_store.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

using ecuio_consent::kStoreMaxBytes;
using ecuio_consent::kStoreMaxEntries;
using ecuio_consent::kStoreMaxUtc;
using ecuio_consent::record;
using ecuio_consent::store_io;

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* What a store that read holds, by the harness's own reading of the
 * rules: each record inside its bounds, each fingerprint once */
void check_records(const std::vector<record>& list)
{
    check(list.size() <= kStoreMaxEntries);
    for (size_t k = 0; k < list.size(); ++k) {
        const record& r = list[k];
        const std::string& plain = r.file ? r.name : r.publisher;
        if (r.file) {
            check(r.id.empty() && r.version.empty() && r.publisher.empty());
        } else {
            check(!r.id.empty() && r.id.size() <= 31);
            check(!r.version.empty() && r.version.size() <= 15);
            check(r.name.empty());
        }
        check(!plain.empty() && plain.size() <= 127);
        for (const char c : plain) {
            check(static_cast<unsigned char>(c) >= 0x20 && c != 0x7F);
        }
        check(r.given_utc >= 1 && r.given_utc <= kStoreMaxUtc);
        for (size_t j = 0; j < k; ++j) {
            check(memcmp(list[j].fingerprint, r.fingerprint, 32) != 0);
        }
    }
}

} // namespace

/* Arbitrary bytes through the store's reader; what reads is written and
 * read again to the same records */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::vector<record> list;
    std::string why;
    const store_io io = ecuio_consent::store_read(
        reinterpret_cast<const char*>(data), size, &list, &why);
    if (io != store_io::ok) {
        check(list.empty());
        check(io == store_io::out_of_memory || !why.empty());
        check(why.find('\n') == std::string::npos);
        return 0;
    }
    check(size <= kStoreMaxBytes);
    check_records(list);
    const std::string text = ecuio_consent::store_text(list);
    check(text.size() <= kStoreMaxBytes);
    std::vector<record> back;
    check(ecuio_consent::store_read(text.data(), text.size(), &back, &why) ==
          store_io::ok);
    check(back.size() == list.size());
    for (size_t k = 0; k < list.size(); ++k) {
        const record& a = list[k];
        const record& b = back[k];
        check(a.id == b.id && a.version == b.version &&
              a.publisher == b.publisher && a.given_utc == b.given_utc &&
              a.file == b.file && a.name == b.name &&
              memcmp(a.fingerprint, b.fingerprint, 32) == 0 &&
              memcmp(a.terms_sha256, b.terms_sha256, 32) == 0);
    }
    return 0;
}
