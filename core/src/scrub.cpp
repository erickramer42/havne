// File/layer: core/src/scrub.cpp | C++17, Qt-free; the classifier and the
//             check of a scrubbed export (ADR-0054 section 1)
// Testing: UNIT: ScrubTest, ExportTest, FolderExportTest, HealthTest
//          (test_core)

#include "scrub.h"

#include "ecuio/recording.h"

#include <cstring>

namespace ecuio_scrub {

namespace {

/* A prefilter over a form's first four bytes, so most of a file's
 * positions cost a shift and a bit test */
constexpr uint32_t kFilterBits = 1u << 16;

uint32_t key_at(const uint8_t* p)
{
    uint32_t k = 0;
    std::memcpy(&k, p, sizeof k);
    return k;
}

uint32_t filter_slot(uint32_t key)
{
    return (key * 2654435761u) >> 16;
}

bool pad(uint8_t c)
{
    return c == 0x00 || c == 0xFF || c == ' ';
}

bool alnum(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

/* ISO 3779's alphabet: capitals but I, O and Q, and the digits */
bool vin_char(uint8_t c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q');
}

std::string hex(const std::string& b, bool upper, bool spaced)
{
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 3);
    for (size_t i = 0; i < b.size(); ++i) {
        if (spaced && i > 0) {
            out += ' ';
        }
        const auto c = static_cast<uint8_t>(b[i]);
        out += digits[c >> 4];
        out += digits[c & 0x0F];
    }
    return out;
}

} // namespace

bool reply_id(uint32_t id, bool extended)
{
    if (extended) {
        return (id & 0x1FFFFF00u) == 0x18DAF100u;
    }
    return id >= 0x7E8 && id <= 0x7EF;
}

bool identification(const uint8_t* msg, size_t n)
{
    if (n >= 1 && msg[0] == 0x49) {
        return true;
    }
    return n >= 3 && msg[0] == 0x62 && msg[1] == 0xF1 && msg[2] >= 0x80;
}

void identification_values(const uint8_t* msg, size_t n,
                           std::vector<std::string>& out)
{
    if (!identification(msg, n) || n <= 3) {
        return;
    }
    /* 49 <item> <count> <data>, 62 <DID> <data> */
    out.emplace_back(reinterpret_cast<const char*>(msg + 3), n - 3);
}

bool values::add(const void* bytes, size_t n, uint32_t cls)
{
    const auto* b = static_cast<const uint8_t*>(bytes);
    size_t i = 0;
    while (i < n) {
        while (i < n && (b[i] == 0x00 || b[i] == 0xFF)) {
            ++i;
        }
        size_t j = i;
        while (j < n && b[j] != 0x00 && b[j] != 0xFF) {
            ++j;
        }
        size_t s = i;
        size_t e = j;
        while (s < e && pad(b[s])) {
            ++s;
        }
        while (e > s && pad(b[e - 1])) {
            --e;
        }
        if (e - s >= kValueMin) {
            const std::string piece(reinterpret_cast<const char*>(b + s),
                                    e - s);
            if (forms_.size() + 5 > kFormsMax) {
                return false;
            }
            add_form(piece, cls);
            add_form(hex(piece, true, false), cls);
            add_form(hex(piece, false, false), cls);
            add_form(hex(piece, true, true), cls);
            add_form(hex(piece, false, true), cls);
        }
        i = j;
    }
    return true;
}

void values::add_form(std::string bytes, uint32_t cls)
{
    if (!seen_.insert(bytes).second) {
        return;
    }
    const uint32_t k = key_at(reinterpret_cast<const uint8_t*>(bytes.data()));
    if (filter_.empty()) {
        filter_.assign(kFilterBits, false);
    }
    filter_[filter_slot(k)] = true;
    by_key_[k].push_back(static_cast<uint32_t>(forms_.size()));
    if (bytes.size() > longest_) {
        longest_ = bytes.size();
    }
    forms_.push_back(form{std::move(bytes), cls});
}

uint32_t values::find(const uint8_t* p, size_t n) const
{
    if (forms_.empty() || n < kValueMin) {
        return 0;
    }
    for (size_t i = 0; i + kValueMin <= n; ++i) {
        const uint32_t k = key_at(p + i);
        if (!filter_[filter_slot(k)]) {
            continue;
        }
        const auto it = by_key_.find(k);
        if (it == by_key_.end()) {
            continue;
        }
        for (uint32_t f : it->second) {
            const std::string& s = forms_[f].bytes;
            if (s.size() <= n - i &&
                std::memcmp(p + i, s.data(), s.size()) == 0) {
                return forms_[f].cls + 1;
            }
        }
    }
    return 0;
}

void check::feed(const uint8_t* p, size_t n)
{
    if (found_ != 0 || n == 0) {
        return;
    }
    for (size_t i = 0; i < n && found_ == 0; ++i) {
        vin_byte(p[i]);
    }
    if (found_ != 0) {
        return;
    }
    const size_t keep = values_.longest() > 1 ? values_.longest() - 1 : 0;
    if (keep == 0) {
        return;
    }
    std::string buf;
    buf.reserve(tail_.size() + n);
    buf = tail_;
    buf.append(reinterpret_cast<const char*>(p), n);
    found_ =
        values_.find(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
    tail_ = buf.size() > keep ? buf.substr(buf.size() - keep) : buf;
}

uint32_t check::finish()
{
    if (found_ == 0) {
        vin_end();
    }
    return found_;
}

void check::vin_byte(uint8_t c)
{
    if (!alnum(c)) {
        vin_end();
        return;
    }
    ++run_;
    run_vin_ = run_vin_ && vin_char(c);
    run_letter_ = run_letter_ || (c >= 'A' && c <= 'Z');
    run_digit_ = run_digit_ || (c >= '0' && c <= '9');
    run_hex_ = run_hex_ && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
}

void check::vin_end()
{
    if (run_ == kVinLength && run_vin_ && run_letter_ && run_digit_ &&
        !run_hex_) {
        found_ = ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1;
    }
    run_ = 0;
    run_vin_ = true;
    run_letter_ = false;
    run_digit_ = false;
    run_hex_ = true;
}

uint32_t check_bytes(const values& v, const uint8_t* p, size_t n)
{
    check c(v);
    c.feed(p, n);
    return c.finish();
}

const char* failed_words(uint32_t failed_class)
{
    switch (failed_class) {
    case ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1:
        return "an identification reply's value";
    case ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES + 1:
        return "a text signal's value";
    case ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1:
        return "a VIN-shaped token";
    default:
        return "a value the export leaves out";
    }
}

namespace {

/* The query keys a scrubbed URI keeps: core's own, each value a number,
 * an ID, a DID list or a profile's name. `serial` is ELM327's
 * serial-port rate (elm327_transport.cpp), a number, never a device's
 * serial. */
bool kept_query_key(const std::string& key)
{
    static const char* const kKeys[] = {
        "profile", "did",   "baud",  "serial",   "filter",
        "txid",    "poll",  "probe", "identify", "diagnose",
        "pack",    "speed", "loop",  "from",     "to"};
    for (const char* k : kKeys) {
        if (key == k) {
            return true;
        }
    }
    return false;
}

bool plain_value(const std::string& v)
{
    for (char c : v) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || std::strchr(":,./_-", c) != nullptr)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string uri(const std::string& uri)
{
    const size_t scheme_end = uri.find("://");
    if (scheme_end == std::string::npos) {
        return "<address>";
    }
    const std::string scheme = uri.substr(0, scheme_end);
    const std::string rest = uri.substr(scheme_end + 3);
    const char* ph = (scheme == "replay" || scheme == "j2534") ? "<path>"
                     : scheme == "mock"                        ? "<label>"
                                                               : "<address>";
    std::string out = scheme + "://" + ph;
    const size_t q = rest.find('?');
    if (q == std::string::npos) {
        return out;
    }
    std::string kept;
    size_t at = q + 1;
    while (at <= rest.size()) {
        size_t amp = rest.find('&', at);
        if (amp == std::string::npos) {
            amp = rest.size();
        }
        const std::string pair = rest.substr(at, amp - at);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && kept_query_key(pair.substr(0, eq)) &&
            plain_value(pair.substr(eq + 1))) {
            kept += (kept.empty() ? "?" : "&") + pair;
        }
        at = amp + 1;
    }
    return out + kept;
}

} // namespace ecuio_scrub
