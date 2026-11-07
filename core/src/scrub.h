// File/layer: core/src/scrub.h | internal, C++17, Qt-free; what a scrubbed
//             export leaves out, and the check of what it wrote (ADR-0054
//             section 1)
// Testing: UNIT: ScrubTest, ExportTest, FolderExportTest, HealthTest
//          (test_core)
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
// that aren't letters or digits, or any 17 such characters inside a
// longer run of letters and digits whose ninth is their check digit
// (vin.h's ECUIO_VIN_CHECK_MATCHES). A match is the class of the value it
// matched, plus one (ECUIO_EXPORT_LEFT_OUT_*), never the value.

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
};

/* The check over bytes as they're written, in pieces: a form or a VIN
 * across two pieces is found */
class check
{
public:
    explicit check(const values& v) : values_(v) {}

    /* Each piece in order; once a match is found the rest is ignored */
    void feed(const uint8_t* p, size_t n);
    /* The end of the bytes: the class plus one of the first match, or 0 */
    uint32_t finish();
    uint32_t found() const { return found_; }

private:
    void vin_byte(uint8_t c);
    void vin_end();
    bool vin_window() const;

    const values& values_;
    std::string tail_;
    uint32_t found_ = 0;
    /* The run of letters and digits under way */
    size_t run_ = 0;
    bool run_vin_ = true;
    bool run_letter_ = false;
    bool run_digit_ = false;
    bool run_hex_ = true;
    /* The run's last characters of the VIN alphabet in a row, the newest
     * kVinLength of them in a ring */
    size_t vin_run_ = 0;
    char ring_[kVinLength] = {};
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
