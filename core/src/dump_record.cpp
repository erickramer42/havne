// File/layer: core/src/dump_record.cpp | internal, C++17, Qt-free, every
//             platform; no file is touched here
// Testing: UNIT: DumpFileTest (test_core); FUZZ: fuzz_dump_file
//
// A dump file's header and record in memory (dump_file.h): read strictly,
// written by the calls a reader reads back.

#include "dump_file.h"

#include "json.h"
#include "plugin_loader.h"
#include "plugin_rules.h"
#include "sha256.h"
#include "signal_file_build.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>

namespace ecuio_dump_file {

namespace {

using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;
using ecuio_plugin_loader::plain_of;
using ecuio_sf::expect;
using ecuio_sf::refuse;
using ecuio_sf::text_of;
using ecuio_sf::whole_of;

constexpr char kFormatKey[] = "havne_dump";
constexpr unsigned kRecordDepth = 8;

uint32_t le32(const uint8_t* p)
{
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) |
           (uint32_t{p[3]} << 24);
}

uint64_t le64(const uint8_t* p)
{
    return uint64_t{le32(p)} | (uint64_t{le32(p + 4)} << 32);
}

void put32(uint8_t* p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

/* Every key of obj is one of allowed: a record takes no key of a
 * tool's own, since its bytes are sealed */
void exact_keys(const Value& obj, std::initializer_list<const char*> allowed,
                const char* context)
{
    for (const Member& m : obj.members) {
        bool known = false;
        for (const char* k : allowed) {
            known = known || m.key == k;
        }
        if (!known) {
            refuse(m, "unknown key " + ecuio_sf::q(m.key) + " in " + context);
        }
    }
}

const Value& needed(const Value& obj, const char* key, const char* context)
{
    const Value* v = obj.find(key);
    if (v == nullptr) {
        refuse(obj, std::string(context) + " needs \"" + key + "\"");
    }
    return *v;
}

int hex_value(char c, bool upper)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (upper ? (c >= 'A' && c <= 'F') : (c >= 'a' && c <= 'f')) {
        return c - (upper ? 'A' : 'a') + 10;
    }
    return -1;
}

/* v's text as lo to hi bytes of hex, upper- or lower-case */
std::vector<uint8_t> hex_of(const Value& v, size_t lo, size_t hi, bool upper,
                            const char* what)
{
    const std::string& t = text_of(v, 2 * lo, 2 * hi, what);
    std::vector<uint8_t> out;
    out.reserve(t.size() / 2);
    for (size_t i = 0; i < t.size(); i += 2) {
        const int a = hex_value(t[i], upper);
        const int b = i + 1 < t.size() ? hex_value(t[i + 1], upper) : -1;
        if (a < 0 || b < 0) {
            refuse(v, std::string(what) + " takes pairs of 0-9 and " +
                          (upper ? "A-F" : "a-f"));
        }
        out.push_back(static_cast<uint8_t>(a * 16 + b));
    }
    return out;
}

void digest_of(const Value& v, const char* what, uint8_t out[32])
{
    const std::vector<uint8_t> b = hex_of(v, 32, 32, false, what);
    std::memcpy(out, b.data(), 32);
}

/* A CAN ID: 3 upper-case hex digits within 11 bits, or 8 within 29 */
uint32_t id_of(const Value& v, const char* what)
{
    const std::string& t = text_of(v, 3, 8, what);
    uint32_t id = 0;
    for (char c : t) {
        const int d = hex_value(c, true);
        if (d < 0 || (t.size() != 3 && t.size() != 8)) {
            id = UINT32_MAX;
            break;
        }
        id = id * 16 + static_cast<uint32_t>(d);
    }
    if (id > (t.size() == 3 ? 0x7FFu : 0x1FFFFFFFu)) {
        refuse(v, std::string(what) +
                      " takes 3 hex digits (11-bit) or 8 (29-bit), A-F in "
                      "capitals");
    }
    return id;
}

/* One of words, its place from 1 */
uint32_t word_of(const Value& v, std::initializer_list<const char*> words,
                 const char* what)
{
    expect(v, Type::string, what);
    uint32_t at = 1;
    std::string list;
    for (const char* w : words) {
        if (v.text == w) {
            return at;
        }
        list += std::string(at == 1 ? "" : ", ") + "\"" + w + "\"";
        ++at;
    }
    refuse(v, std::string(what) + " is one of " + list);
}

uint32_t u32_of(const Value& v, int64_t hi, const char* what)
{
    return static_cast<uint32_t>(whole_of(v, 0, hi, what));
}

const std::string& id_text(const Value& v, size_t hi, const char* what)
{
    const std::string& t = text_of(v, 1, hi, what);
    if (!ecuio_plugin_rules::id_ok(t, hi)) {
        refuse(v, std::string(what) +
                      " takes a-z, 0-9 and '-', a letter or digit at each "
                      "end");
    }
    return t;
}

range range_of(const Value& v)
{
    expect(v, Type::object, "a range");
    exact_keys(v, {"name", "step", "session", "address", "bytes", "offset"},
               "a range");
    range r;
    r.name = id_text(needed(v, "name", "a range"), kNameMax, "a range's name");
    r.step = word_of(needed(v, "step", "a range"), {"read", "upload"},
                     "a range's step");
    /* the words in the order of the sessions' own bytes, 02 then 03 */
    r.session = 1 + word_of(needed(v, "session", "a range"),
                            {"programming", "extended"}, "a range's session");
    const Value& address = needed(v, "address", "a range");
    const std::vector<uint8_t> a =
        hex_of(address, 4, 4, true, "a range's address");
    for (uint8_t b : a) {
        r.address = (r.address << 8) | b;
    }
    r.bytes = static_cast<uint64_t>(whole_of(needed(v, "bytes", "a range"), 1,
                                             ECUIO_DUMP_IMAGE_MAX,
                                             "a range's bytes"));
    r.offset = static_cast<uint64_t>(whole_of(needed(v, "offset", "a range"), 0,
                                              ECUIO_DUMP_IMAGE_MAX,
                                              "a range's offset"));
    if (r.address + r.bytes > (uint64_t{1} << 32)) {
        refuse(address, "a range runs past the module's 32-bit addresses");
    }
    return r;
}

void read_ranges(const Value& list, const header& h, record& r)
{
    expect(list, Type::array, "\"ranges\"");
    if (list.items.empty() || list.items.size() > ECUIO_DUMP_MAX_RANGES) {
        refuse(list, "\"ranges\" lists " + std::to_string(list.items.size()) +
                         "; a dump holds 1 to " +
                         std::to_string(ECUIO_DUMP_MAX_RANGES));
    }
    uint64_t next = 0;
    for (const Value& v : list.items) {
        range g = range_of(v);
        if (g.offset != next) {
            refuse(v, "the ranges don't tile the image: this one starts at " +
                          std::to_string(g.offset) + ", not " +
                          std::to_string(next));
        }
        next += g.bytes;
        for (const range& known : r.ranges) {
            if (g.address < known.address + known.bytes &&
                known.address < g.address + g.bytes) {
                refuse(v, "two ranges overlap in address");
            }
        }
        r.ranges.push_back(std::move(g));
    }
    if (next != h.image_bytes) {
        refuse(list, "the ranges don't tile the image: they add to " +
                         std::to_string(next) + " bytes and the image has " +
                         std::to_string(h.image_bytes));
    }
}

void read_idents(const Value& list, record& r)
{
    expect(list, Type::array, "\"ident\"");
    if (list.items.size() > ECUIO_DUMP_MAX_IDENTS) {
        refuse(list, "\"ident\" lists " + std::to_string(list.items.size()) +
                         ", more than " +
                         std::to_string(ECUIO_DUMP_MAX_IDENTS));
    }
    for (const Value& v : list.items) {
        expect(v, Type::object, "an identification");
        exact_keys(v, {"did", "role", "hex"}, "an identification");
        ident d;
        const std::vector<uint8_t> did =
            hex_of(needed(v, "did", "an identification"), 2, 2, true,
                   "an identification's did");
        d.did = (uint32_t{did[0]} << 8) | did[1];
        d.role = word_of(needed(v, "role", "an identification"),
                         {"hardware", "software", "base"},
                         "an identification's role");
        d.value = hex_of(needed(v, "hex", "an identification"), 1,
                         ECUIO_DUMP_IDENT_MAX, true, "an identification's hex");
        for (const ident& known : r.idents) {
            if (known.did == d.did) {
                refuse(v, "an identification's did is given twice");
            }
        }
        r.idents.push_back(std::move(d));
    }
}

void read_obd(const Value& v, record& r)
{
    expect(v, Type::object, "\"obd\"");
    exact_keys(v, {"vin", "calids", "cvns", "name"}, "\"obd\"");
    if (v.members.empty()) {
        refuse(v, "\"obd\" holds nothing: a dump leaves it out instead");
    }
    if (const Value* vin = v.find("vin")) {
        r.vin = text_of(*vin, 17, 17, "\"obd\"'s vin");
        for (char c : r.vin) {
            const bool ok =
                (c >= '0' && c <= '9') ||
                (c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q');
            if (!ok) {
                refuse(*vin, "\"obd\"'s vin takes 17 digits and capital "
                             "letters, never I, O or Q");
            }
        }
    }
    const auto list_of = [](const Value& list, const char* what) {
        expect(list, Type::array, what);
        if (list.items.empty() ||
            list.items.size() > ECUIO_DUMP_MAX_CALIBRATIONS) {
            refuse(list, std::string(what) + " lists " +
                             std::to_string(list.items.size()) +
                             "; it takes 1 to " +
                             std::to_string(ECUIO_DUMP_MAX_CALIBRATIONS));
        }
    };
    if (const Value* calids = v.find("calids")) {
        list_of(*calids, "\"calids\"");
        for (const Value& c : calids->items) {
            r.calids.push_back(plain_of(c, 1, kCalidMax, "a calid", false));
        }
    }
    if (const Value* cvns = v.find("cvns")) {
        list_of(*cvns, "\"cvns\"");
        for (const Value& c : cvns->items) {
            const std::vector<uint8_t> b = hex_of(c, 4, 4, true, "a cvn");
            r.cvns.push_back({b[0], b[1], b[2], b[3]});
        }
    }
    if (const Value* name = v.find("name")) {
        r.ecu_name = plain_of(*name, 1, kEcuNameMax, "\"obd\"'s name", false);
    }
}

void read_module(const Value& v, record& r)
{
    expect(v, Type::object, "\"module\"");
    exact_keys(v, {"rx", "tx", "ident", "obd"}, "\"module\"");
    const Value& rx = needed(v, "rx", "\"module\"");
    const Value& tx = needed(v, "tx", "\"module\"");
    r.rx_id = id_of(rx, "the module's rx");
    r.tx_id = id_of(tx, "the module's tx");
    if (rx.text.size() != tx.text.size()) {
        refuse(tx, "the module's rx and tx differ in width");
    }
    read_idents(needed(v, "ident", "\"module\""), r);
    if (const Value* obd = v.find("obd")) {
        read_obd(*obd, r);
    }
}

void read_plugin(const Value& v, record& r)
{
    expect(v, Type::object, "\"plugin\"");
    exact_keys(v,
               {"id", "version", "module_sha256", "fingerprint", "operation"},
               "\"plugin\"");
    r.plugin_id = id_text(needed(v, "id", "\"plugin\""), ECUIO_PLUGIN_ID_MAX,
                          "the plugin's id");
    const Value& version = needed(v, "version", "\"plugin\"");
    r.plugin_version = text_of(version, 1, 15, "the plugin's version");
    uint32_t parts[3];
    if (!ecuio_plugin_rules::parse_version(r.plugin_version, parts)) {
        refuse(version, "the plugin's version isn't X.Y.Z");
    }
    digest_of(needed(v, "module_sha256", "\"plugin\""),
              "the plugin's module_sha256", r.module_sha256);
    digest_of(needed(v, "fingerprint", "\"plugin\""),
              "the plugin's fingerprint", r.fingerprint);
    r.operation = id_text(needed(v, "operation", "\"plugin\""), kNameMax,
                          "the plugin's operation");
}

void read_transport(const Value& v, record& r)
{
    expect(v, Type::object, "\"transport\"");
    exact_keys(v, {"scheme", "adapter", "firmware"}, "\"transport\"");
    const Value& scheme = needed(v, "scheme", "\"transport\"");
    r.scheme = text_of(scheme, 1, kSchemeMax, "the transport's scheme");
    for (char c : r.scheme) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            refuse(scheme, "the transport's scheme takes a-z and 0-9");
        }
    }
    r.adapter = plain_of(needed(v, "adapter", "\"transport\""), 0, kTextMax,
                         "the transport's adapter", false);
    r.firmware = plain_of(needed(v, "firmware", "\"transport\""), 0, kTextMax,
                          "the transport's firmware", false);
}

void read_read(const Value& v, record& r)
{
    expect(v, Type::object, "\"read\"");
    exact_keys(v,
               {"passes", "seconds", "volts_mv", "volts_min_mv", "rpm", "kmh",
                "parked_tick", "unlocks", "writes_before"},
               "\"read\"");
    const Value& passes = needed(v, "passes", "\"read\"");
    r.passes = u32_of(passes, 2, "\"read\"'s passes");
    if (r.passes != 2) {
        refuse(passes, "\"read\"'s passes is 2: a copy is read twice");
    }
    r.seconds = u32_of(needed(v, "seconds", "\"read\""), UINT32_MAX,
                       "\"read\"'s seconds");
    const auto maybe = [&v](const char* key, uint32_t hi, bool* has,
                            uint32_t* out) {
        if (const Value* x = v.find(key)) {
            *has = true;
            *out = u32_of(*x, hi, key);
        }
    };
    maybe("volts_mv", kVoltsMax, &r.has_volts, &r.volts_mv);
    maybe("volts_min_mv", kVoltsMax, &r.has_volts_min, &r.volts_min_mv);
    maybe("rpm", kSpeedMax, &r.has_rpm, &r.rpm);
    maybe("kmh", kSpeedMax, &r.has_kmh, &r.kmh);
    const Value& tick = needed(v, "parked_tick", "\"read\"");
    expect(tick, Type::boolean, "\"read\"'s parked_tick");
    r.parked_tick = tick.boolean;
    r.unlocks = u32_of(needed(v, "unlocks", "\"read\""), kUnlocksMax,
                       "\"read\"'s unlocks");
    r.writes_before = u32_of(needed(v, "writes_before", "\"read\""), UINT32_MAX,
                             "\"read\"'s writes_before");
}

/* The format is judged before the keys, so a later format's record is
 * refused as that */
void read_root(const Value& root, const header& h, record& r)
{
    expect(root, Type::object, "a dump's record");
    const Value& format = needed(root, kFormatKey, "a dump's record");
    if (whole_of(format, 0, INT32_MAX, "\"havne_dump\"") != h.format) {
        refuse(format, "\"havne_dump\" isn't the header's format");
    }
    exact_keys(root,
               {kFormatKey, "kind", "utc", "core", "image", "ranges", "module",
                "plugin", "transport", "read"},
               "a dump's record");
    const Value& kind = needed(root, "kind", "a dump's record");
    expect(kind, Type::string, "\"kind\"");
    if (kind.text != "read") {
        refuse(kind, "this Havne reads a dump of kind \"read\" only");
    }
    r.kind = ECUIO_DUMP_KIND_READ;
    r.utc =
        whole_of(needed(root, "utc", "a dump's record"), 1, kMaxUtc, "\"utc\"");
    const Value& core = needed(root, "core", "a dump's record");
    r.core = text_of(core, 1, 15, "\"core\"");
    uint32_t parts[3];
    if (!ecuio_plugin_rules::parse_version(r.core, parts)) {
        refuse(core, "\"core\" isn't X.Y.Z");
    }
    const Value& image = needed(root, "image", "a dump's record");
    expect(image, Type::object, "\"image\"");
    exact_keys(image, {"bytes", "sha256"}, "\"image\"");
    const Value& bytes = needed(image, "bytes", "\"image\"");
    r.image_bytes = static_cast<uint64_t>(
        whole_of(bytes, 1, ECUIO_DUMP_IMAGE_MAX, "the image's bytes"));
    if (r.image_bytes != h.image_bytes) {
        refuse(bytes, "the record's image is " + std::to_string(r.image_bytes) +
                          " bytes and the header's " +
                          std::to_string(h.image_bytes));
    }
    const Value& sha = needed(image, "sha256", "\"image\"");
    digest_of(sha, "the image's sha256", r.image_sha256);
    if (std::memcmp(r.image_sha256, h.image_sha256, 32) != 0) {
        refuse(sha, "the record's image checksum isn't the header's");
    }
    read_ranges(needed(root, "ranges", "a dump's record"), h, r);
    read_module(needed(root, "module", "a dump's record"), r);
    read_plugin(needed(root, "plugin", "a dump's record"), r);
    read_transport(needed(root, "transport", "a dump's record"), r);
    read_read(needed(root, "read", "a dump's record"), r);
}

Value number(uint64_t n)
{
    Value v;
    v.type = Type::number;
    v.integer = true;
    v.in_int64 = true;
    v.whole = static_cast<int64_t>(n);
    v.number = static_cast<double>(n);
    return v;
}

Value string_of(std::string t)
{
    Value v;
    v.type = Type::string;
    v.text = std::move(t);
    return v;
}

Value boolean(bool b)
{
    Value v;
    v.type = Type::boolean;
    v.boolean = b;
    return v;
}

Value object()
{
    Value v;
    v.type = Type::object;
    return v;
}

Value array()
{
    Value v;
    v.type = Type::array;
    return v;
}

void put(Value& obj, const char* key, Value v)
{
    Member m;
    m.key = key;
    m.value = std::move(v);
    obj.members.push_back(std::move(m));
}

std::string upper_hex(const uint8_t* p, size_t n)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string s;
    s.reserve(2 * n);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 0x0F]);
    }
    return s;
}

std::string id_hex(uint32_t id, bool wide)
{
    char text[16];
    std::snprintf(text, sizeof text, wide ? "%08X" : "%03X", id);
    return text;
}

Value module_value(const record& r)
{
    const bool wide = r.rx_id > 0x7FFu || r.tx_id > 0x7FFu;
    Value module = object();
    put(module, "rx", string_of(id_hex(r.rx_id, wide)));
    put(module, "tx", string_of(id_hex(r.tx_id, wide)));
    Value idents = array();
    for (const ident& d : r.idents) {
        constexpr const char* roles[] = {"hardware", "software", "base"};
        const uint8_t did[2] = {static_cast<uint8_t>(d.did >> 8),
                                static_cast<uint8_t>(d.did)};
        Value v = object();
        put(v, "did", string_of(upper_hex(did, 2)));
        put(v, "role", string_of(roles[(d.role - 1) % 3]));
        put(v, "hex", string_of(upper_hex(d.value.data(), d.value.size())));
        idents.items.push_back(std::move(v));
    }
    put(module, "ident", std::move(idents));
    Value obd = object();
    if (!r.vin.empty()) {
        put(obd, "vin", string_of(r.vin));
    }
    if (!r.calids.empty()) {
        Value list = array();
        for (const std::string& c : r.calids) {
            list.items.push_back(string_of(c));
        }
        put(obd, "calids", std::move(list));
    }
    if (!r.cvns.empty()) {
        Value list = array();
        for (const std::array<uint8_t, 4>& c : r.cvns) {
            list.items.push_back(string_of(upper_hex(c.data(), c.size())));
        }
        put(obd, "cvns", std::move(list));
    }
    if (!r.ecu_name.empty()) {
        put(obd, "name", string_of(r.ecu_name));
    }
    if (!obd.members.empty()) {
        put(module, "obd", std::move(obd));
    }
    return module;
}

Value read_value(const record& r)
{
    Value read = object();
    put(read, "passes", number(r.passes));
    put(read, "seconds", number(r.seconds));
    if (r.has_volts) {
        put(read, "volts_mv", number(r.volts_mv));
    }
    if (r.has_volts_min) {
        put(read, "volts_min_mv", number(r.volts_min_mv));
    }
    if (r.has_rpm) {
        put(read, "rpm", number(r.rpm));
    }
    if (r.has_kmh) {
        put(read, "kmh", number(r.kmh));
    }
    put(read, "parked_tick", boolean(r.parked_tick));
    put(read, "unlocks", number(r.unlocks));
    put(read, "writes_before", number(r.writes_before));
    return read;
}

/* y, m, d of the days since 1970-01-01 (Howard Hinnant's civil_from_days) */
void civil_of(int64_t days, int* y, unsigned* m, unsigned* d)
{
    days += 719468;
    const int64_t era = days / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = static_cast<int>(static_cast<int64_t>(yoe) + era * 400) +
         (*m <= 2 ? 1 : 0);
}

judged refused_with(std::string* why, std::string words)
{
    *why = std::move(words);
    return judged::refused;
}

} // namespace

judged read_header(const uint8_t* bytes, uint64_t file_bytes, header* out,
                   std::string* why)
{
    try {
        if (std::memcmp(bytes, kMagic, sizeof kMagic) != 0) {
            return refused_with(why, "it isn't a dump file: its first bytes "
                                     "aren't a dump's");
        }
        header h;
        h.format = le32(bytes + 8);
        if (h.format > ECUIO_DUMP_FORMAT) {
            return refused_with(why, "it was made by a later Havne (format " +
                                         std::to_string(h.format) +
                                         "): this one reads format " +
                                         std::to_string(ECUIO_DUMP_FORMAT));
        }
        if (h.format == 0) {
            return refused_with(why, "its format is 0");
        }
        if (le32(bytes + 12) != kHeaderBytes) {
            return refused_with(why, "its header isn't 128 bytes");
        }
        h.image_bytes = le64(bytes + 16);
        h.record_bytes = le32(bytes + 24);
        if (le32(bytes + 28) != 0) {
            return refused_with(why, "its header's flags aren't zero");
        }
        for (size_t i = 96; i < kHeaderBytes; ++i) {
            if (bytes[i] != 0) {
                return refused_with(why,
                                    "its header's reserved bytes aren't zero");
            }
        }
        if (h.image_bytes == 0 || h.image_bytes > ECUIO_DUMP_IMAGE_MAX) {
            return refused_with(why, "its image is " +
                                         std::to_string(h.image_bytes) +
                                         " bytes: a dump holds 1 to " +
                                         std::to_string(ECUIO_DUMP_IMAGE_MAX));
        }
        if (h.record_bytes == 0 || h.record_bytes > ECUIO_DUMP_RECORD_MAX) {
            return refused_with(why, "its record is " +
                                         std::to_string(h.record_bytes) +
                                         " bytes: a dump's is 1 to " +
                                         std::to_string(ECUIO_DUMP_RECORD_MAX));
        }
        const uint64_t whole = kHeaderBytes + h.image_bytes + h.record_bytes;
        if (file_bytes != whole) {
            return refused_with(
                why, "its sizes don't add up: the header says " +
                         std::to_string(whole) + " bytes and the file has " +
                         std::to_string(file_bytes));
        }
        std::memcpy(h.image_sha256, bytes + 32, 32);
        std::memcpy(h.record_sha256, bytes + 64, 32);
        *out = h;
        return judged::ok;
    } catch (const std::bad_alloc&) {
        return judged::out_of_memory;
    }
}

void write_header(const header& h, uint8_t out[kHeaderBytes])
{
    std::memset(out, 0, kHeaderBytes);
    std::memcpy(out, kMagic, sizeof kMagic);
    put32(out + 8, h.format);
    put32(out + 12, static_cast<uint32_t>(kHeaderBytes));
    put32(out + 16, static_cast<uint32_t>(h.image_bytes));
    put32(out + 20, static_cast<uint32_t>(h.image_bytes >> 32));
    put32(out + 24, h.record_bytes);
    std::memcpy(out + 32, h.image_sha256, 32);
    std::memcpy(out + 64, h.record_sha256, 32);
}

judged read_record(const char* text, size_t size, const header& h, record* out,
                   std::string* why)
{
    try {
        *out = record{};
        uint8_t sum[32];
        ecuio_sha256::hasher hash;
        hash.add(text, size);
        hash.finish(sum);
        if (std::memcmp(sum, h.record_sha256, 32) != 0) {
            return refused_with(why,
                                "its record isn't the one its header seals");
        }
        Value root;
        ecuio_json::Error error;
        ecuio_json::Limits limits;
        limits.max_bytes = ECUIO_DUMP_RECORD_MAX;
        limits.max_depth = kRecordDepth;
        if (!ecuio_json::parse(text, size, &root, &error, limits)) {
            if (error.out_of_memory) {
                return judged::out_of_memory;
            }
            return refused_with(why, "its record, " + error.to_string());
        }
        try {
            read_root(root, h, *out);
        } catch (const ecuio_sf::refused& r) {
            *out = record{};
            return refused_with(why, "its record, line " +
                                         std::to_string(r.line) + ", column " +
                                         std::to_string(r.column) + ": " +
                                         r.words);
        }
        return judged::ok;
    } catch (const std::bad_alloc&) {
        *out = record{};
        return judged::out_of_memory;
    }
}

std::string record_text(const record& r)
{
    Value root = object();
    put(root, kFormatKey, number(ECUIO_DUMP_FORMAT));
    put(root, "kind", string_of("read"));
    put(root, "utc", number(static_cast<uint64_t>(r.utc)));
    put(root, "core", string_of(r.core));
    Value image = object();
    put(image, "bytes", number(r.image_bytes));
    put(image, "sha256", string_of(ecuio_sha256::hex(r.image_sha256)));
    put(root, "image", std::move(image));
    Value ranges = array();
    for (const range& g : r.ranges) {
        const uint8_t address[4] = {static_cast<uint8_t>(g.address >> 24),
                                    static_cast<uint8_t>(g.address >> 16),
                                    static_cast<uint8_t>(g.address >> 8),
                                    static_cast<uint8_t>(g.address)};
        Value v = object();
        put(v, "name", string_of(g.name));
        put(v, "step",
            string_of(g.step == ECUIO_DUMP_STEP_UPLOAD ? "upload" : "read"));
        put(v, "session",
            string_of(g.session == ECUIO_DUMP_SESSION_PROGRAMMING
                          ? "programming"
                          : "extended"));
        put(v, "address", string_of(upper_hex(address, 4)));
        put(v, "bytes", number(g.bytes));
        put(v, "offset", number(g.offset));
        ranges.items.push_back(std::move(v));
    }
    put(root, "ranges", std::move(ranges));
    put(root, "module", module_value(r));
    Value plugin = object();
    put(plugin, "id", string_of(r.plugin_id));
    put(plugin, "version", string_of(r.plugin_version));
    put(plugin, "module_sha256", string_of(ecuio_sha256::hex(r.module_sha256)));
    put(plugin, "fingerprint", string_of(ecuio_sha256::hex(r.fingerprint)));
    put(plugin, "operation", string_of(r.operation));
    put(root, "plugin", std::move(plugin));
    Value transport = object();
    put(transport, "scheme", string_of(r.scheme));
    put(transport, "adapter", string_of(r.adapter));
    put(transport, "firmware", string_of(r.firmware));
    put(root, "transport", std::move(transport));
    put(root, "read", read_value(r));
    return ecuio_json::write(root);
}

bool same(const record& a, const record& b)
{
    return record_text(a) == record_text(b);
}

std::string name_of(int64_t utc, uint32_t tx_id, const uint8_t file_sha256[32])
{
    int y = 1970;
    unsigned m = 1;
    unsigned d = 1;
    civil_of(utc / 86400, &y, &m, &d);
    char head[32];
    std::snprintf(head, sizeof head, "%04d%02u%02u-", y, m, d);
    return head + id_hex(tx_id, tx_id > 0x7FFu) + "-" +
           ecuio_sha256::hex(file_sha256) + kSuffix;
}

bool name_hash(std::string_view name, uint8_t out[32])
{
    constexpr size_t suffix = sizeof kSuffix - 1;
    /* 8 digits, '-', 3 or 8 hex, '-', 64 hex, the suffix */
    const size_t tx = name.size() == 8 + 1 + 3 + 1 + 64 + suffix   ? 3
                      : name.size() == 8 + 1 + 8 + 1 + 64 + suffix ? 8
                                                                   : 0;
    if (tx == 0 || name.substr(name.size() - suffix) != kSuffix ||
        name[8] != '-' || name[9 + tx] != '-') {
        return false;
    }
    for (size_t i = 0; i < 8; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    for (size_t i = 0; i < tx; ++i) {
        if (hex_value(name[9 + i], true) < 0) {
            return false;
        }
    }
    const size_t at = 10 + tx;
    for (size_t i = 0; i < 32; ++i) {
        const int a = hex_value(name[at + 2 * i], false);
        const int b = hex_value(name[at + 2 * i + 1], false);
        if (a < 0 || b < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>(a * 16 + b);
    }
    return true;
}

bool partial_name(std::string_view name)
{
    const std::string mark = std::string(kSuffix) + ".partial-";
    return name.find(mark) != std::string_view::npos;
}

std::string build(record r, const uint8_t* image, size_t image_bytes)
{
    header h;
    h.image_bytes = image_bytes;
    ecuio_sha256::hasher hash;
    hash.add(image, image_bytes);
    hash.finish(h.image_sha256);
    r.image_bytes = image_bytes;
    std::memcpy(r.image_sha256, h.image_sha256, 32);
    const std::string text = record_text(r);
    h.record_bytes = static_cast<uint32_t>(text.size());
    hash.add(text.data(), text.size());
    hash.finish(h.record_sha256);
    uint8_t head[kHeaderBytes];
    write_header(h, head);
    std::string file;
    file.reserve(kHeaderBytes + image_bytes + text.size());
    file.append(reinterpret_cast<const char*>(head), kHeaderBytes);
    file.append(reinterpret_cast<const char*>(image), image_bytes);
    file.append(text);
    return file;
}

judged parse(const uint8_t* bytes, size_t size, header* h, record* r,
             std::string* why)
{
    if (size < kHeaderBytes) {
        try {
            return refused_with(why, "it is shorter than a dump's header");
        } catch (const std::bad_alloc&) {
            return judged::out_of_memory;
        }
    }
    const judged head = read_header(bytes, size, h, why);
    if (head != judged::ok) {
        return head;
    }
    const uint8_t* image = bytes + kHeaderBytes;
    const judged body =
        read_record(reinterpret_cast<const char*>(image + h->image_bytes),
                    h->record_bytes, *h, r, why);
    if (body != judged::ok) {
        return body;
    }
    uint8_t sum[32];
    ecuio_sha256::hasher hash;
    hash.add(image, static_cast<size_t>(h->image_bytes));
    hash.finish(sum);
    if (std::memcmp(sum, h->image_sha256, 32) != 0) {
        try {
            return refused_with(why,
                                "its image isn't the one its header seals");
        } catch (const std::bad_alloc&) {
            return judged::out_of_memory;
        }
    }
    return judged::ok;
}

} // namespace ecuio_dump_file
