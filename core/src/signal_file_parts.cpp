// File/layer: core/src/signal_file_parts.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest, PluginTest (test_signals);
//          FUZZ: fuzz_signal_file
//
// A signal file's pieces (ADR-0051), read for build: the ECUs, a
// signal's request and field, an enum's labels, a gauge, a group.

#include "ecuio/signals.h"
#include "json.h"
#include "least_core.h"
#include "signal_file_build.h"
#include "signal_file_parts.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_sf {

using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;

void read_gauge(const Value& s, const ecuio_signal_def_t& d, uint32_t def,
                ecuio_signal_file& f, const std::string& what)
{
    const Value* v = s.find("gauge");
    if (v == nullptr) {
        return;
    }
    ecuio_least::raise(f.least, ecuio_least::kGauge);
    const std::string gauge = what + "'s gauge";
    expect(*v, Type::object, gauge);
    only_keys(*v, {"min", "max", "warn_low", "warn_high"}, what + "'s gauge");
    const Value* min_v = v->find("min");
    const Value* max_v = v->find("max");
    if (min_v == nullptr || max_v == nullptr) {
        refuse(*v, gauge + " needs \"min\" and \"max\"");
    }
    using ecuio_json::number_text;
    ecuio_signal_gauge_t g{};
    g.min = finite_of(*min_v, gauge + " min");
    g.max = finite_of(*max_v, gauge + " max");
    if (!(g.min < g.max)) {
        refuse(*max_v, gauge + " max " + number_text(g.max) +
                           " isn't above its min " + number_text(g.min));
    }
    const std::string span = number_text(g.min) + " to " + number_text(g.max);
    const auto level = [&](const char* key, uint32_t flag, double& out) {
        const Value* w = v->find(key);
        if (w != nullptr) {
            out = finite_of(*w, gauge + " " + key);
            if (out < g.min || out > g.max) {
                refuse(*w, gauge + " " + key + " " + number_text(out) +
                               " is outside its range, " + span);
            }
            g.flags |= flag;
        }
        return w;
    };
    level("warn_low", ECUIO_GAUGE_WARN_LOW, g.warn_low);
    const Value* high = level("warn_high", ECUIO_GAUGE_WARN_HIGH, g.warn_high);
    if (g.flags == (ECUIO_GAUGE_WARN_LOW | ECUIO_GAUGE_WARN_HIGH) &&
        !(g.warn_low < g.warn_high)) {
        refuse(*high, gauge + " warn_high " + number_text(g.warn_high) +
                          " isn't above its warn_low " +
                          number_text(g.warn_low));
    }
    /* A wider range than the field's is a choice; none in common is a
     * needle that never moves, a computed def having no field to judge */
    double lo = 0.0;
    double hi = 0.0;
    if (ecuio_def_range(&d, &lo, &hi) == ECUIO_DECODE_OK &&
        (g.max < lo || g.min > hi)) {
        const std::string unit =
            d.unit[0] != '\0' ? q(d.unit) : std::string("the signal's");
        refuse(*v, gauge + " runs " + span + ", but its field reads only " +
                       number_text(lo) + " to " + number_text(hi) +
                       ": is the gauge in another unit than " + unit + "?");
    }
    f.gauges.emplace_back(def, g);
}

/* Hex digits only, digits_lo to digits_hi of them */
uint32_t hex_of(const Value& v, size_t digits_lo, size_t digits_hi,
                const std::string& what)
{
    expect(v, Type::string, what);
    const std::string& s = v.text;
    if (s.size() < digits_lo || s.size() > digits_hi) {
        refuse(v, what + " " + q(s) + " takes " +
                      (digits_lo == digits_hi
                           ? std::to_string(digits_lo)
                           : std::to_string(digits_lo) + " to " +
                                 std::to_string(digits_hi)) +
                      " hex digits");
    }
    uint32_t n = 0;
    for (char c : s) {
        uint32_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint32_t>(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint32_t>(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint32_t>(c - 'a' + 10);
        } else {
            refuse(v, what + " " + q(s) + " isn't hex");
        }
        n = (n << 4) | d;
    }
    return n;
}

bool is_11_bit(uint32_t id)
{
    return id <= 0x7FFu;
}

/* ADR-0020's tx, only where it's the standard's: rx - 8 for 0x7E8 to
 * 0x7EF, the 18DA swap for 18DAF1xx; 0 elsewhere */
uint32_t standard_tx(uint32_t rx)
{
    if (rx >= 0x7E8u && rx <= 0x7EFu) {
        return rx - 8u;
    }
    if ((rx & 0xFFFFFF00u) == 0x18DAF100u) {
        return 0x18DA00F1u | ((rx & 0xFFu) << 8);
    }
    return 0;
}

uint8_t group_of(const Value& v, const std::string& what)
{
    expect(v, Type::string, what);
    for (uint32_t g = 0; g <= ECUIO_SIGNAL_GROUP_DIAGNOSTICS; ++g) {
        std::string word = ecuio_signal_group_name(g);
        for (char& c : word) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        if (v.text == word) {
            return static_cast<uint8_t>(g);
        }
    }
    refuse(v, what + " " + q(v.text) +
                  " isn't a group: engine, air, fuel, oxygen sensors, "
                  "emissions, vehicle, electrical, diagnostics or other");
}

namespace {

/* ecu id's tx: as given, of its rx's width, else the standard's for rx */
uint32_t tx_of(const Value& e, const std::string& id, uint32_t rx)
{
    if (const Value* tx_v = e.find("tx")) {
        const uint32_t tx = hex_of(*tx_v, 1, 8, "ecu " + q(id) + "'s tx");
        if (tx == 0 || tx > 0x1FFFFFFFu) {
            refuse(*tx_v, "ecu " + q(id) + "'s tx is outside 1 to 1FFFFFFF");
        }
        if (is_11_bit(tx) != is_11_bit(rx)) {
            refuse(*tx_v,
                   "ecu " + q(id) + "'s tx is another width than its rx");
        }
        return tx;
    }
    const uint32_t tx = standard_tx(rx);
    if (tx == 0) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "%X", static_cast<unsigned>(rx));
        refuse(e, "ecu " + q(id) + ": rx " + hex + " needs a tx");
    }
    return tx;
}

/* An ECU's "name", "" with none: a label a screen shows for the module */
std::string name_of(const Value& e, const std::string& id, ecuio_signal_file& f)
{
    const Value* name = e.find("name");
    if (name == nullptr) {
        return std::string();
    }
    ecuio_least::raise(f.least, ecuio_least::kEcuName);
    const std::string what = "ecu " + q(id) + "'s name";
    const std::string& text = text_of(*name, 1, 63, what);
    no_formula(*name, what);
    return text;
}

/* An ECU's "session": the one its signals are read in */
uint8_t session_of(const Value& e, const std::string& id, ecuio_signal_file& f)
{
    const Value* session = e.find("session");
    if (session == nullptr) {
        return ECUIO_CHANNEL_SESSION_NONE;
    }
    ecuio_least::raise(f.least, ecuio_least::kEcuSession);
    const std::string what = "ecu " + q(id) + "'s session";
    const std::string& text = text_of(*session, 1, 63, what);
    if (text == "extended") {
        return ECUIO_CHANNEL_SESSION_EXTENDED;
    }
    if (text == "default") {
        return ECUIO_CHANNEL_SESSION_DEFAULT;
    }
    if (text == "programming" || text == "02") {
        refuse(*session, what + " can't be the programming session: it is "
                                "core's alone, entered only by a write "
                                "operation, never by a signal file");
    }
    refuse(*session,
           what + " is " + q(text) + ": it takes \"default\" or \"extended\"");
}

/* An ECU's "part" in a merged set: the part whose id stands for it */
uint32_t part_of(const Value& e, const std::string& id,
                 const ecuio_signal_file& f)
{
    const Value* part = e.find("part");
    if (part == nullptr) {
        return ECUIO_SIGNAL_PART_NONE;
    }
    if (f.parts.empty()) {
        refuse(*part, "ecu " + q(id) +
                          "'s \"part\" is a merged set's: a "
                          "file with no \"parts\" takes none");
    }
    return static_cast<uint32_t>(
        whole_of(*part, 0, static_cast<int64_t>(f.parts.size()) - 1,
                 "ecu " + q(id) + "'s part"));
}

/* An ECU's "session_parts" in a merged set: the parts asking it extended */
std::vector<uint32_t> session_parts_of(const Value& e, const std::string& id,
                                       const ecuio_signal_file& f)
{
    std::vector<uint32_t> out;
    const Value* list = e.find("session_parts");
    if (list == nullptr) {
        return out;
    }
    if (f.parts.empty()) {
        refuse(*list, "ecu " + q(id) +
                          "'s \"session_parts\" is a merged set's: a file "
                          "with no \"parts\" takes none");
    }
    expect(*list, Type::array, "ecu " + q(id) + "'s session_parts");
    for (const Value& p : list->items) {
        out.push_back(static_cast<uint32_t>(
            whole_of(p, 0, static_cast<int64_t>(f.parts.size()) - 1,
                     "ecu " + q(id) + "'s session part")));
    }
    return out;
}

/* A channel for ecu id, of the width the set carries, within its count */
void add_channel(const Value& e, const std::string& id, uint32_t rx,
                 uint32_t tx, ecuio_signal_file& f)
{
    if (f.channels.size() >= ECUIO_MAX_CHANNELS) {
        refuse(e, "ecu " + q(id) + " is past the " +
                      std::to_string(ECUIO_MAX_CHANNELS) +
                      " ECUs one session reads, the base's counted");
    }
    for (const ecuio_channel_decl_t& c : f.channels) {
        if (is_11_bit(c.rx_id) != is_11_bit(rx)) {
            refuse(e, "ecu " + q(id) + " is " +
                          (is_11_bit(rx) ? "11-bit" : "29-bit") + " beside " +
                          (is_11_bit(rx) ? "29-bit" : "11-bit") +
                          " IDs: one session carries one width");
        }
    }
    ecuio_channel_decl_t c{};
    c.rx_id = rx;
    c.tx_id = tx;
    f.channels.push_back(c);
    f.modules.emplace_back();
}

} // namespace

void read_ecus(const Value* ecus, ecuio_signal_file& f,
               std::vector<std::pair<std::string, uint32_t>>* ids)
{
    f.modules.resize(f.channels.size());
    if (ecus == nullptr) {
        return;
    }
    expect(*ecus, Type::array, "\"ecus\"");
    for (const Value& e : ecus->items) {
        expect(e, Type::object, "an ECU");
        only_keys(
            e, {"id", "rx", "tx", "name", "session", "session_parts", "part"},
            "an ECU");
        const Value* id_v = e.find("id");
        if (id_v == nullptr) {
            refuse(e, "an ECU needs an \"id\"");
        }
        const std::string id = text_of(*id_v, 1, 31, "an ECU's id");
        if (!key_ok(id)) {
            refuse(*id_v, "ECU id " + q(id) + " takes 1 to 31 of a-z, 0-9 and _");
        }
        for (const auto& known : *ids) {
            if (known.first == id) {
                refuse(*id_v, "ECU id " + q(id) + " is given twice");
            }
        }
        const Value* rx_v = e.find("rx");
        if (rx_v == nullptr) {
            refuse(e, "ecu " + q(id) + " needs an \"rx\"");
        }
        const uint32_t rx = hex_of(*rx_v, 1, 8, "ecu " + q(id) + "'s rx");
        if (rx == 0 || rx > 0x1FFFFFFFu) {
            refuse(*rx_v, "ecu " + q(id) + "'s rx is outside 1 to 1FFFFFFF");
        }
        const uint32_t tx = tx_of(e, id, rx);
        size_t channel = 0; /* the one at its rx, or the count */
        while (channel < f.channels.size() && f.channels[channel].rx_id != rx) {
            ++channel;
        }
        if (channel < f.channels.size() && f.channels[channel].tx_id != tx) {
            refuse(e, "ecu " + q(id) +
                          " shares its rx with another "
                          "ECU but not its tx");
        }
        for (const auto& known : *ids) {
            if (known.second == rx) {
                refuse(e, "ecu " + q(id) + " shares its rx with ecu " +
                              q(known.first));
            }
        }
        if (channel == f.channels.size()) {
            add_channel(e, id, rx, tx, f);
        }
        f.modules[channel].id = id;
        f.modules[channel].name = name_of(e, id, f);
        f.channels[channel].session = session_of(e, id, f);
        f.modules[channel].part = part_of(e, id, f);
        f.modules[channel].session_parts = session_parts_of(e, id, f);
        ids->emplace_back(id, rx);
    }
}

void ecu_addresses(const Value& ecu, uint32_t* rx, uint32_t* tx)
{
    *rx = hex_of(*ecu.find("rx"), 1, 8, "an ECU's rx");
    const Value* given = ecu.find("tx");
    *tx = given != nullptr ? hex_of(*given, 1, 8, "an ECU's tx")
                           : standard_tx(*rx);
}

void read_field(const Value& s, const std::string& kind,
                const std::vector<std::pair<std::string, uint32_t>>& ecus,
                const ecuio_signal_file& f, ecuio_signal_def_t& d,
                const std::string& what)
{
    uint32_t rx = 0;
    if (const Value* ecu_v = s.find("ecu")) {
        const std::string id = text_of(*ecu_v, 1, 31, what + "'s ecu");
        for (const auto& e : ecus) {
            if (e.first == id) {
                rx = e.second;
            }
        }
        if (rx == 0) {
            refuse(*ecu_v, what + " reads ecu " + q(id) + ", which \"ecus\" doesn't list");
        }
    } else if (!ecus.empty()) {
        rx = ecus.front().second;
    } else if (!f.channels.empty()) {
        rx = f.channels.front().rx_id;
    } else {
        refuse(s, what + " reads a reply, but the file lists no ECU");
    }
    d.message_id = rx;

    const Value* read = s.find("read");
    if (read == nullptr) {
        refuse(s, what + " needs \"read\"");
    }
    expect(*read, Type::object, what + "'s read");
    only_keys(*read, {"service", "number"}, what + "'s read");
    const Value* service = read->find("service");
    const Value* number = read->find("number");
    if (service == nullptr || number == nullptr) {
        refuse(*read, what + "'s read needs \"service\" and \"number\"");
    }
    expect(*service, Type::string, what + "'s service");
    uint16_t echo = 0;
    if (service->text == "01") {
        d.mux_length = 2;
        d.mux_key = 0x4100u | hex_of(*number, 2, 2, what + "'s PID");
        echo = 2;
    } else if (service->text == "22") {
        d.mux_length = 3;
        d.mux_key = 0x620000u | hex_of(*number, 4, 4, what + "'s DID");
        echo = 3;
    } else {
        refuse(*service, what + "'s service " + q(service->text) +
                             " isn't \"01\" or \"22\"");
    }

    const Value* field = s.find("field");
    if (field == nullptr) {
        refuse(s, what + " needs \"field\"");
    }
    expect(*field, Type::object, what + "'s field");
    const bool text = kind == "text" || kind == "hex";
    if (text) {
        only_keys(*field, {"byte", "bytes"}, what + "'s field");
    } else if (kind == "enum") {
        only_keys(*field, {"byte", "bits", "bit", "order"}, what + "'s field");
    } else {
        only_keys(*field, {"byte", "bits", "bit", "order", "signed"},
                  what + "'s field");
    }
    const Value* byte = field->find("byte");
    if (byte == nullptr) {
        refuse(*field, what + "'s field needs \"byte\"");
    }
    d.start_byte = static_cast<uint16_t>(
        echo + whole_of(*byte, 0, ECUIO_FRAME_DATA_MAX - 1 - echo, what + "'s byte"));
    if (text) {
        if (const Value* bytes = field->find("bytes")) {
            d.byte_length = static_cast<uint16_t>(
                whole_of(*bytes, 0, ECUIO_FRAME_DATA_MAX, what + "'s bytes"));
            if (uint32_t{d.start_byte} + d.byte_length > ECUIO_FRAME_DATA_MAX) {
                refuse(*field, what + "'s field ends past the " +
                                   std::to_string(ECUIO_FRAME_DATA_MAX) +
                                   " bytes a reply holds");
            }
        }
        d.kind = static_cast<uint8_t>(kind == "hex" ? ECUIO_VALUE_HEX : ECUIO_VALUE_TEXT);
        d.byte_order = ECUIO_BYTE_ORDER_BE;
        return;
    }
    d.bit_length = 8;
    if (const Value* bits = field->find("bits")) {
        d.bit_length = static_cast<uint8_t>(whole_of(*bits, 1, 32, what + "'s bits"));
    }
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    if (const Value* order = field->find("order")) {
        expect(*order, Type::string, what + "'s order");
        if (order->text == "le") {
            d.byte_order = ECUIO_BYTE_ORDER_LE;
        } else if (order->text != "be") {
            refuse(*order, what + "'s order " + q(order->text) +
                               " isn't \"be\" or \"le\"");
        }
    }
    if (const Value* bit = field->find("bit")) {
        d.bit_offset = static_cast<uint8_t>(whole_of(*bit, 0, 7, what + "'s bit"));
    }
    if (d.byte_order == ECUIO_BYTE_ORDER_BE &&
        (d.bit_offset != 0 || d.bit_length % 8 != 0)) {
        refuse(*field, what + "'s field is big-endian, so whole bytes: bits a "
                              "multiple of 8 and no bit (\"order\": \"le\" for a "
                              "bit field)");
    }
    if (uint32_t{d.start_byte} + (d.bit_offset + d.bit_length + 7u) / 8u >
        ECUIO_FRAME_DATA_MAX) {
        refuse(*field, what + "'s field ends past the " +
                           std::to_string(ECUIO_FRAME_DATA_MAX) +
                           " bytes a reply holds");
    }
    bool is_signed = false;
    if (const Value* sv = field->find("signed")) {
        expect(*sv, Type::boolean, what + "'s signed");
        is_signed = sv->boolean;
    }
    d.kind = static_cast<uint8_t>(kind == "enum" ? ECUIO_VALUE_ENUM
                                  : is_signed      ? ECUIO_VALUE_INT
                                                   : ECUIO_VALUE_UINT);
}

void read_values(const Value& s, ecuio_signal_file& f, ecuio_signal_def_t& d,
                 const std::string& what)
{
    const Value* values = s.find("values");
    if (values == nullptr) {
        refuse(s, what + " is an enum and needs \"values\"");
    }
    expect(*values, Type::object, what + "'s values");
    if (values->members.size() > ECUIO_ENUM_MAX_ENTRIES) {
        refuse(*values, what + " has " + std::to_string(values->members.size()) +
                            " values; an enum holds at most 256");
    }
    const uint64_t top = d.bit_length == 32 ? 0xFFFFFFFFull
                                            : ((1ull << d.bit_length) - 1);
    std::vector<ecuio_enum_entry_t> mine;
    for (const Member& m : values->members) {
        if (m.key.empty() || m.key.size() > 10 ||
            m.key.find_first_not_of("0123456789") != std::string::npos ||
            (m.key.size() > 1 && m.key[0] == '0')) {
            refuse(m, what + "'s value " + q(m.key) + " isn't a decimal number");
        }
        const uint64_t raw = std::stoull(m.key);
        if (raw > top) {
            refuse(m, what + "'s value " + m.key + " can't be in its " +
                          std::to_string(d.bit_length) + "-bit field");
        }
        const std::string label =
            text_of(m.value, 1, ECUIO_ENUM_LABEL_MAX, what + "'s label for " + m.key);
        no_formula(m.value, what + "'s label");
        ecuio_enum_entry_t e{};
        e.raw = static_cast<uint32_t>(raw);
        copy_text(e.label, sizeof e.label, label);
        mine.push_back(e);
    }
    std::sort(mine.begin(), mine.end(),
              [](const ecuio_enum_entry_t& a, const ecuio_enum_entry_t& b) {
                  return a.raw < b.raw;
              });
    if (f.enums.size() + mine.size() > ECUIO_SET_MAX_TABLE) {
        refuse(*values, "the file's enums hold more than 65536 labels");
    }
    d.table_first = static_cast<uint16_t>(f.enums.size());
    d.table_count = static_cast<uint16_t>(mine.size());
    f.enums.insert(f.enums.end(), mine.begin(), mine.end());
}

} // namespace ecuio_sf
