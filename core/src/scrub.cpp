// File/layer: core/src/scrub.cpp | C++17, Qt-free; the classifier and the
//             check of a scrubbed export (ADR-0054 section 1)
// Testing: UNIT: ScrubTest, ScrubMaskTest, ExportTest, FolderExportTest,
//          HealthTest (test_core)

#include "scrub.h"

#include "ecuio/recording.h"
#include "ecuio/vin.h"

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

int hex_value(uint8_t c)
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

constexpr uint32_t kVinShaped = ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1;

/* The first form of v in tail and [p, p + n), as the bytes stream: the
 * class plus one, or 0; tail keeps what a form across the next piece
 * needs */
uint32_t search(const values& v, const uint8_t* p, size_t n, std::string& tail)
{
    const size_t keep = v.longest() > 1 ? v.longest() - 1 : 0;
    if (keep == 0) {
        return 0;
    }
    std::string buf;
    buf.reserve(tail.size() + n);
    buf = tail;
    buf.append(reinterpret_cast<const char*>(p), n);
    const uint32_t found =
        v.find(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
    tail = buf.size() > keep ? buf.substr(buf.size() - keep) : buf;
    return found;
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

bool segment_reply_id(uint32_t id, bool extended)
{
    if (extended) {
        return (id & 0x1FFF0000u) == 0x18DA0000u;
    }
    return reply_id(id, false);
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
            starred_ = starred_ || piece.find(kMaskChar) != std::string::npos;
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
    if (found_ != 0 || ended_ || n == 0) {
        return;
    }
    /* The recording's own values first: one found names its class, where
     * a VIN's shape (a DID echoing the VIN, say) would name only the shape,
     * and a mask would hide it */
    found_ = search(*values_, p, n, tail_);
    if (found_ == 0) {
        out_ = nullptr;
        feed_vin(p, n);
    }
}

void check::feed_masking(uint8_t* p, size_t n, size_t held)
{
    if (found_ != 0 || ended_ || n == 0) {
        return;
    }
    found_ = search(*values_, p, n, tail_);
    if (found_ == 0) {
        out_ = p;
        out_at_ = at_;
        out_held_ = held;
        feed_vin(p, n);
        out_ = nullptr;
    }
}

void check::written(const uint8_t* p, size_t n)
{
    if (found_ == 0 && (mode_ & kMask) != 0 && values_->starred()) {
        found_ = search(*values_, p, n, written_tail_);
    }
}

uint32_t check::finish()
{
    if (!ended_ && found_ == 0) {
        out_ = nullptr;
        end_vin();
    }
    ended_ = true;
    return found_;
}

uint32_t check::finish_masking(uint8_t* end, size_t held)
{
    if (!ended_ && found_ == 0) {
        out_ = end;
        out_at_ = at_;
        out_held_ = held;
        end_vin();
        out_ = nullptr;
    }
    ended_ = true;
    return found_;
}

bool check::window_in(const uint8_t* p, size_t n)
{
    check c;
    c.feed_vin(p, n);
    c.end_vin();
    return c.found_ != 0;
}

void check::feed_vin(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n && found_ == 0; ++i, ++at_) {
        if ((mode_ & kJson) == 0 || !json_byte(p[i])) {
            vin_byte(p[i], at_);
        }
    }
}

void check::end_vin()
{
    if (escape_ == 2) {
        replay(at_ - pending_n_);
    }
    escape_ = 0;
    vin_end(at_);
}

/* Byte at_'s part in a JSON escape: true when it's taken here. The escaped
 * character is a separator, as the backslash; a \u's four digits are one
 * too, or the bytes as they are when it stands for a letter or a digit. */
bool check::json_byte(uint8_t c)
{
    if (escape_ == 0) {
        if (c != '\\') {
            return false;
        }
        vin_end(at_);
        escape_ = 1;
        return true;
    }
    if (escape_ == 1) {
        escape_ = c == 'u' ? 2 : 0;
        pending_[0] = c;
        pending_n_ = c == 'u' ? 1 : 0;
        code_ = 0;
        return true;
    }
    const int d = hex_value(c);
    if (d < 0) {
        escape_ = 0;
        replay(at_ - pending_n_);
        return json_byte(c);
    }
    pending_[pending_n_++] = c;
    code_ = code_ * 16 + static_cast<uint32_t>(d);
    if (pending_n_ < sizeof pending_) {
        return true;
    }
    escape_ = 0;
    if (code_ < 0x80 && alnum(static_cast<uint8_t>(code_))) {
        replay(at_ + 1 - pending_n_);
    }
    pending_n_ = 0;
    return true;
}

void check::replay(uint64_t first)
{
    const size_t n = pending_n_;
    pending_n_ = 0;
    for (size_t k = 0; k < n && found_ == 0; ++k) {
        vin_byte(pending_[k], first + k);
    }
}

void check::reset_run()
{
    run_ = 0;
    vin_run_ = 0;
    run_vin_ = true;
    run_letter_ = false;
    run_digit_ = false;
    run_hex_ = true;
}

uint8_t* check::byte_at(uint64_t at) const
{
    if (out_ == nullptr || at + out_held_ < out_at_) {
        return nullptr;
    }
    return at >= out_at_ ? out_ + (at - out_at_)
                         : out_ - static_cast<ptrdiff_t>(out_at_ - at);
}

/* A mask the bytes it covers can't take (feed, or past the held) fails
 * the check: never a token left as it is */
void check::mask(uint64_t from, uint64_t to)
{
    if (from < to && byte_at(from) == nullptr) {
        found_ = kVinShaped;
        return;
    }
    for (uint64_t i = from; i < to; ++i) {
        *byte_at(i) = static_cast<uint8_t>(kMaskChar);
    }
}

/* Bytes [from, to) of the run under way, alone between masks: a VIN by the
 * shape rule */
bool check::shaped(uint64_t from, uint64_t to) const
{
    bool letter = false;
    bool digit = false;
    bool hex = true;
    for (uint64_t i = from; i < to; ++i) {
        const uint8_t* b = byte_at(i);
        if (b == nullptr) {
            return true; /* masked, or failed by mask() */
        }
        if (!vin_char(*b)) {
            return false;
        }
        letter = letter || (*b >= 'A' && *b <= 'Z');
        digit = digit || (*b >= '0' && *b <= '9');
        hex = hex && hex_value(*b) >= 0;
    }
    return letter && digit && !hex;
}

void check::vin_byte(uint8_t c, uint64_t at)
{
    if (!alnum(c)) {
        vin_end(at);
        return;
    }
    if (whole_run_) {
        found_ = kVinShaped;
        return;
    }
    if (ahead_ > 0) {
        if (vin_char(c)) {
            mask(at, at + 1);
            --ahead_;
            return;
        }
        ahead_ = 0;
    }
    if (run_ == 0) {
        run_at_ = at;
    }
    ++run_;
    run_vin_ = run_vin_ && vin_char(c);
    run_letter_ = run_letter_ || (c >= 'A' && c <= 'Z');
    run_digit_ = run_digit_ || (c >= '0' && c <= '9');
    run_hex_ = run_hex_ && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
    if (!vin_char(c)) {
        vin_run_ = 0;
        return;
    }
    if (vin_run_ == 0) {
        vin_at_ = at;
    }
    ring_[vin_run_ % kVinLength] = static_cast<char>(c);
    ++vin_run_;
    if (vin_run_ >= kVinLength && vin_window()) {
        window_hit(at);
    }
}

/* A window ending at byte at. Masked: from 16 before its start, clipped to
 * the run of the VIN alphabet, to at, then 16 after as they come; what's
 * left of the run before the span is a run of its own, as written. */
void check::window_hit(uint64_t at)
{
    if (window_only_ && run_ == kVinLength) {
        whole_run_ = true; /* a window only if the run goes on */
        return;
    }
    if ((mode_ & kMask) == 0) {
        found_ = kVinShaped;
        return;
    }
    const uint64_t back = 2 * (kVinLength - 1);
    const uint64_t from = at >= back && at - back > vin_at_ ? at - back : vin_at_;
    if (from - run_at_ == kVinLength && shaped(run_at_, from)) {
        ++masked_;
        mask(run_at_, from);
    }
    ++masked_;
    mask(from, at + 1);
    reset_run();
    ahead_ = kVinLength - 1;
}

/* The newest kVinLength characters of the VIN alphabet: a VIN's shape, and
 * position 9 their check digit or their maker code one vPIC lists. A run
 * of exactly 17 is a VIN by its shape alone (vin_end); in a longer run, a
 * VIN that a letter or digit either side hides is found by its check digit
 * (other text of the shape matches about one window in 33) or, as a VIN
 * outside North America needs none, by its maker code (about one in 22
 * more). vPIC lists the codes of vehicles sold in the US: a VIN with no
 * check digit whose code it doesn't list is missed here. */
bool check::vin_window() const
{
    char w[kVinLength + 1];
    bool letter = false;
    bool digit = false;
    bool hex = true;
    for (size_t i = 0; i < kVinLength; ++i) {
        const char c = ring_[(vin_run_ + i) % kVinLength];
        w[i] = c;
        letter = letter || (c >= 'A' && c <= 'Z');
        digit = digit || (c >= '0' && c <= '9');
        hex = hex && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
    }
    w[kVinLength] = '\0';
    if (!letter || !digit || hex) {
        return false;
    }
    ecuio_vin_info_t info{};
    if (ecuio_vin_decode(w, 0, &info) != ECUIO_OK) {
        return false;
    }
    return (info.flags & ECUIO_VIN_CHECK_MATCHES) != 0 ||
           *ecuio_vin_make(info.wmi) != '\0';
}

/* The run under way ends at byte at: a run of exactly 17 is the shape
 * rule's, masked whole under kMask */
void check::vin_end(uint64_t at)
{
    if (!window_only_ && run_ == kVinLength && run_vin_ && run_letter_ &&
        run_digit_ && !run_hex_) {
        if ((mode_ & kMask) != 0) {
            ++masked_;
            mask(run_at_, at);
        } else {
            found_ = kVinShaped;
        }
    }
    reset_run();
    ahead_ = 0;
    whole_run_ = false;
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
        "profile", "did",          "baud",       "serial",   "filter",
        "txid",    "poll",         "probe",      "identify", "diagnose",
        "pack",    "speed",        "loop",       "from",     "to",
        "session", "keepalive_ms", "packframes", "hold"};
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
