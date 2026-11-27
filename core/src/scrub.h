// File/layer: core/src/scrub.h | internal, C++17, Qt-free; what a scrubbed
//             export leaves out, and the check of what it wrote (ADR-0054
//             section 1)
// Testing: UNIT: ScrubTest, ScrubMaskTest, ExportTest, FolderExportTest,
//          HealthTest (test_core)
//
// Scrubbed by construction, then checked. The classifier names what never
// goes out as it is (an identification reply, a segment whose message
// can't be reassembled); every export format asks it. The check then
// searches what was written, and each message as it was before any
// segmenting, for the recording's own identity values (`values`: each
// identification reply's text and bytes, each text signal's values) in
// every form an export writes bytes in (as they are, and as hex upper or
// lower case, spaced or not), and for any VIN-shaped token: 17 of the VIN
// alphabet with a letter and a digit, not all hex, alone between bytes
// that aren't letters or digits (the shape rule), or any 17 such
// characters inside a longer run of letters and digits whose ninth is
// their check digit (vin.h's ECUIO_VIN_CHECK_MATCHES: the window rule). A
// match is the class of the value it matched, plus one
// (ECUIO_EXPORT_LEFT_OUT_*), never the value.
//
// Text a user wrote (names, labels, markers) is checked with kMask: a
// VIN-shaped token there is written as kMaskChar instead of failing the
// export, and counted. A window's mask runs from 16 characters before it to
// 16 after, clipped to its run of the VIN alphabet, so a VIN any window of
// it overlaps is masked whole; a run of exactly 17 is masked whole. The
// check reads the bytes as written, masks included, so the same check over
// a masked file finds nothing. An identity value still fails the export,
// as does any match in bytes core writes (pcapng's, frames.csv's, the
// health report's) and in a message (the plan leaves out a message the
// window rule takes: window_in).

#ifndef ECUIO_SCRUB_H
#define ECUIO_SCRUB_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ecuio_scrub {

/* A value shorter than this, its padding trimmed, turns up by chance in a
 * large file's frames (a CVN's four bytes would, about once in every
 * 4 GiB of them); it isn't searched for. The VIN, calibration IDs and
 * serials are longer. */
constexpr size_t kValueMin = 6;
/* The most distinct forms the check holds: past it, a recording can't be
 * checked, and an export of it fails */
constexpr size_t kFormsMax = 1u << 20;
constexpr size_t kVinLength = 17;
/* What a mask writes: not a letter or digit (so it ends a run), plain in a
 * CSV field and in JSON, and no formula start */
constexpr char kMaskChar = '*';
/* How far back from the byte that finds it a mask reaches: a window's 16,
 * the span's 16 before it, and a run of 17 left before the span, which a
 * mask then ends */
constexpr size_t kMaskReach = 3 * (kVinLength - 1) + 1;

/* check's modes, bits */
constexpr uint32_t kRefuse = 0x0u; /* a VIN-shaped token fails the check */
constexpr uint32_t kMask = 0x1u;   /* it's masked and counted: text a user
                                      wrote, fed through feed_masking */
constexpr uint32_t kJson = 0x2u;   /* JSON: an escape that stands for
                                      anything but a letter or a digit
                                      (\n, \", \u00e9) separates runs, as
                                      the character it stands for does in
                                      the other files */

/* A diagnostic reply ID: 0x7E8-0x7EF, 29-bit 18DAF1xx (ISO 15765-4's
 * replies to the tester at F1) */
bool reply_id(uint32_t id, bool extended);

/* An ID whose segments no message can be attributed to are left out:
 * 0x7E8-0x7EF, or any 29-bit 18DAxxyy (a reply to a tester at another
 * address than F1 too, since the reassembly takes every 18DA ID) */
bool segment_reply_id(uint32_t id, bool extended);

/* An ISO-TP message (its service byte first) that answers an
 * identification request: Service 09, any item, or 62 for a DID in ISO
 * 14229-1's identification block F180-F1FF */
bool identification(const uint8_t* msg, size_t n);

/* The identity values an identification reply carries, each the bytes
 * after its header (49 <item> [count], 62 <DID>) */
void identification_values(const uint8_t* msg, size_t n,
                           std::vector<std::string>& out);

/* The recording's identity values in every form the check searches */
class values
{
public:
    /* Adds a value of class cls: each run of it between NUL, 0xFF or
     * space padding, kValueMin bytes or longer, as it is and as hex.
     * False when the check would hold more than kFormsMax forms. */
    bool add(const void* bytes, size_t n, uint32_t cls);

    size_t forms() const { return forms_.size(); }
    size_t longest() const { return longest_; }
    /* A form holds kMaskChar, so a mask could make one */
    bool starred() const { return starred_; }

    /* The class plus one of the first form found in [p, p + n), else 0 */
    uint32_t find(const uint8_t* p, size_t n) const;

private:
    struct form {
        std::string bytes;
        uint32_t cls = 0;
    };
    void add_form(std::string bytes, uint32_t cls);

    std::vector<form> forms_;
    std::unordered_set<std::string> seen_;
    /* By each form's first four bytes, and a bit per hash of them */
    std::unordered_map<uint32_t, std::vector<uint32_t>> by_key_;
    std::vector<bool> filter_;
    size_t longest_ = 0;
    bool starred_ = false;
};

/* The check over bytes as they're written, in pieces: a form or a VIN
 * across two pieces is found */
class check
{
public:
    explicit check(const values& v, uint32_t mode = kRefuse)
        : values_(&v), mode_(mode)
    {}

    /* Each piece in order; once a match is found the rest is ignored. A
     * mask this check would make fails it instead. */
    void feed(const uint8_t* p, size_t n);
    /* kMask: each piece in order, masked in place. p follows the held
     * bytes fed before it and not yet written in one buffer: every byte
     * fed so far, or the last kMaskReach of them. A mask that would reach
     * past them fails the check. */
    void feed_masking(uint8_t* p, size_t n, size_t held);
    /* kMask: the bytes as they're written, masks made, in order: searched
     * for a value only a mask could make (values::starred) */
    void written(const uint8_t* p, size_t n);
    /* The end of the bytes: the class plus one of the first match, or 0.
     * kMask calls finish_masking before the last bytes are written (a run
     * of 17 at the end is masked there), end the bytes fed, held as
     * feed_masking's; finish then answers what it found. */
    uint32_t finish();
    uint32_t finish_masking(uint8_t* end, size_t held);
    uint32_t found() const { return found_; }
    /* The tokens masked: a span each */
    uint64_t masked() const { return masked_; }

    /* The window rule alone, over one message: a window inside a run
     * longer than 17, which the plan leaves out (a run of exactly 17 is
     * the shape rule's and fails the check) */
    static bool window_in(const uint8_t* p, size_t n);

private:
    /* window_in's: no values, the shape rule off */
    check() : values_(nullptr), mode_(kRefuse), window_only_(true) {}

    void feed_vin(const uint8_t* p, size_t n);
    bool json_byte(uint8_t c);
    void replay(uint64_t first);
    void end_vin();
    uint8_t* byte_at(uint64_t at) const;
    void vin_byte(uint8_t c, uint64_t at);
    void vin_end(uint64_t at);
    void window_hit(uint64_t at);
    bool vin_window() const;
    bool shaped(uint64_t from, uint64_t to) const;
    void mask(uint64_t from, uint64_t to);
    void reset_run();

    const values* values_;
    const uint32_t mode_;
    bool window_only_ = false;
    bool whole_run_ = false; /* window_in's: a window that is its whole run */
    std::string tail_;
    std::string written_tail_;
    uint32_t found_ = 0;
    bool ended_ = false;
    uint64_t masked_ = 0;
    /* The bytes scanned so far, and the buffer feed_masking was given:
     * out_[0] is byte out_at_ of the bytes, out_held_ before it held */
    uint64_t at_ = 0;
    uint8_t* out_ = nullptr;
    uint64_t out_at_ = 0;
    size_t out_held_ = 0;
    /* The run of letters and digits under way, from byte run_at_ */
    size_t run_ = 0;
    uint64_t run_at_ = 0;
    bool run_vin_ = true;
    bool run_letter_ = false;
    bool run_digit_ = false;
    bool run_hex_ = true;
    /* The run's last characters of the VIN alphabet in a row, from byte
     * vin_at_, the newest kVinLength of them in a ring */
    size_t vin_run_ = 0;
    uint64_t vin_at_ = 0;
    char ring_[kVinLength] = {};
    /* A JSON escape under way: 1 after its backslash, 2 in a \u's digits,
     * held in pending_ */
    uint8_t pending_[5] = {};
    bool escaped_ = false;
    bool run_escaped_ = false;
    int escape_ = 0;
    uint32_t code_ = 0;
    uint64_t escape_at_ = 0;
    size_t pending_n_ = 0;
    /* VIN characters still masked after a window's: its span's 16 after */
    size_t ahead_ = 0;
};

/* One message, whole: the class plus one of its first match, or 0 */
uint32_t check_bytes(const values& v, const uint8_t* p, size_t n);

/* The words for a failed check: the class, never the value */
const char* failed_words(uint32_t failed_class);

/* A session's URI as a scrubbed file holds it (ADR-0054 section 1): its
 * scheme, the authority as <path> (a replay's file, a J2534 DLL),
 * <label> (a mock's, the user's text) or <address> (a port, a host and
 * port, a Bluetooth stream, an interface), then only the query pairs
 * core reads whose value is plain (letters, digits and : , . / _ -) */
std::string uri(const std::string& uri);

} // namespace ecuio_scrub

#endif /* ECUIO_SCRUB_H */
