// File/layer: tests/test_signal_keys.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalKeysTest (test_signals): a signal's key (ADR-0065
//          section 9), the address key's text, a set's keys and the def
//          a person's text names

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/profile.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

struct FileFree {
    void operator()(ecuio_signal_file_t* f) const { ecuio_signal_file_free(f); }
};
using File = std::unique_ptr<ecuio_signal_file_t, FileFree>;

File load(const std::string& json)
{
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load(json.data(), json.size(), &f), ECUIO_OK)
        << ecuio_last_error_detail();
    return File(f);
}

/* A number def: its ID, its key's bytes at an offset, its field */
ecuio_signal_def_t number(uint32_t message_id, uint8_t mux_length,
                          uint32_t mux_key, uint16_t start_byte,
                          uint8_t bit_offset, uint8_t bit_length,
                          uint8_t mux_offset = 0)
{
    ecuio_signal_def_t d = {};
    d.message_id = message_id;
    d.mux_offset = mux_offset;
    d.mux_length = mux_length;
    d.mux_key = mux_key;
    d.kind = ECUIO_VALUE_UINT;
    d.byte_order = ECUIO_BYTE_ORDER_LE;
    d.start_byte = start_byte;
    d.bit_offset = bit_offset;
    d.bit_length = bit_length;
    d.scale = 1.0;
    return d;
}

/* A text def: its ID, its key's bytes, its bytes from start_byte */
ecuio_signal_def_t text(uint32_t message_id, uint8_t mux_length,
                        uint32_t mux_key, uint16_t start_byte,
                        uint16_t byte_length, uint8_t kind = ECUIO_VALUE_TEXT)
{
    ecuio_signal_def_t d =
        number(message_id, mux_length, mux_key, start_byte, 0, 0);
    d.kind = kind;
    d.byte_length = byte_length;
    return d;
}

std::string address(const ecuio_signal_def_t& d, ecuio_status_t want = ECUIO_OK)
{
    ecuio_signal_key_t k;
    std::memset(&k, 0x5A, sizeof k);
    EXPECT_EQ(ecuio_signal_address_key(&d, &k), want);
    /* Terminated, and zero from the NUL to the end */
    const size_t n = strnlen(k.text, sizeof k.text);
    EXPECT_LT(n, sizeof k.text);
    for (size_t i = n; i < sizeof k.text; ++i) {
        EXPECT_EQ(k.text[i], '\0') << "byte " << i;
    }
    return std::string(k.text, n);
}

/* The set's keys, every one */
std::vector<std::string> keys_of(const ecuio_signal_set_t* set,
                                 const ecuio_signal_file_t* file)
{
    uint32_t count = 77;
    EXPECT_EQ(ecuio_signal_set_keys(set, file, nullptr, 0, &count), ECUIO_OK);
    EXPECT_EQ(count, set->count);
    std::vector<ecuio_signal_key_t> keys(count);
    uint32_t again = 0;
    EXPECT_EQ(ecuio_signal_set_keys(set, file, keys.data(), count, &again),
              ECUIO_OK);
    EXPECT_EQ(again, count);
    std::vector<std::string> out;
    for (const ecuio_signal_key_t& k : keys) {
        /* Terminated, and zero from the NUL to the end */
        const size_t n = strnlen(k.text, sizeof k.text);
        EXPECT_LT(n, sizeof k.text);
        for (size_t i = n; i < sizeof k.text; ++i) {
            EXPECT_EQ(k.text[i], '\0') << k.text << ", byte " << i;
        }
        out.emplace_back(k.text, n);
    }
    return out;
}

ecuio_signal_set_t set_of(const std::vector<ecuio_signal_def_t>& defs)
{
    ecuio_signal_set_t s = {};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    s.count = static_cast<uint32_t>(defs.size());
    s.defs = defs.data();
    return s;
}

bool author_key(const std::string& k)
{
    if (k.empty() || k.size() > 31) {
        return false;
    }
    for (char c : k) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

} // namespace

/* The text is the def's ID, its key's bytes and its field, and nothing
 * else of the def */
TEST(SignalKeysTest, AnAddressKeyIsTheIdTheKeyBytesAndTheField)
{
    EXPECT_EQ(address(number(0x7E8, 2, 0x410C, 2, 0, 16)), "7E8:410C:2.0:16");
    EXPECT_EQ(address(number(0x7E8, 3, 0x62F40C, 3, 0, 8)), "7E8:62F40C:3.0:8");
    EXPECT_EQ(address(number(0x18DAF110, 3, 0x62D801, 3, 4, 4)),
              "18DAF110:62D801:3.4:4");
    EXPECT_EQ(address(number(0x1A0, 0, 0, 0, 4, 12)), "1A0::0.4:12")
        << "a frame nothing asks: no key bytes";
    EXPECT_EQ(address(number(0x7E8, 1, 0x05, 2, 0, 8, 1)), "7E8:05@1:2.0:8")
        << "a key that doesn't start the payload says where it does";
    EXPECT_EQ(address(number(0x7E8, 2, 0x0005, 2, 0, 8)), "7E8:0005:2.0:8")
        << "every key byte, a zero one too";
    EXPECT_EQ(address(text(0x7E9, 3, 0x62F190, 3, 17)), "7E9:62F190:3:t17");
    EXPECT_EQ(address(text(0x7E9, 3, 0x62F190, 3, 0, ECUIO_VALUE_HEX)),
              "7E9:62F190:3:t0")
        << "to the payload's end";
    EXPECT_EQ(address(number(0x7FF, 0, 0, 0, 0, 8)), "7FF::0.0:8");
    EXPECT_EQ(address(number(0x800, 0, 0, 0, 0, 8)), "00000800::0.0:8")
        << "past 0x7FF an ID is 29-bit";
    EXPECT_EQ(address(number(0, 0, 0, 0, 0, 8)), "000::0.0:8");

    /* A key's bytes past its length are no part of it */
    EXPECT_EQ(address(number(0x7E8, 2, 0xAB410C, 2, 0, 16)), "7E8:410C:2.0:16");

    /* How the bits are read is no part of it */
    ecuio_signal_def_t d = number(0x7E8, 2, 0x410C, 2, 0, 16);
    d.kind = ECUIO_VALUE_INT;
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    d.scale = 0.25;
    d.offset = -40.0;
    d.id = 7001;
    d.plot_rank = 3;
    std::snprintf(d.name, sizeof d.name, "Engine speed");
    std::snprintf(d.unit, sizeof d.unit, "RPM");
    EXPECT_EQ(address(d), "7E8:410C:2.0:16");
    d.kind = ECUIO_VALUE_ENUM;
    EXPECT_EQ(address(d), "7E8:410C:2.0:16");
    /* A text field over the same bytes is another field */
    EXPECT_EQ(address(text(0x7E8, 2, 0x410C, 2, 2)), "7E8:410C:2:t2");

    /* A computed def reads no address */
    ecuio_signal_def_t c = {};
    c.flags = ECUIO_DEF_FLAG_COMPUTED;
    c.id = 7011;
    EXPECT_EQ(address(c), "");

    ecuio_signal_key_t k;
    std::memset(&k, 0x5A, sizeof k);
    EXPECT_EQ(ecuio_signal_address_key(nullptr, &k), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(k.text[0], 0x5A) << "out untouched";
    EXPECT_EQ(ecuio_signal_address_key(&d, nullptr), ECUIO_ERR_NULL_ARGUMENT);
}

/* The longest key of a def the validator takes fills the room exactly;
 * every key starts with a hex digit, so none is a formula to a
 * spreadsheet, and holds a colon, so none is an author's key */
TEST(SignalKeysTest, TheLongestAddressKeyFitsAndNoneIsAnAuthorsKeyOrAFormula)
{
    /* Within a frame: the widest ID, three key bytes at the last offset,
     * the last byte of a frame */
    const std::string in_frame_text =
        address(text(0x18DAF110, 3, 0xFFFFFF, ECUIO_FRAME_DATA_MAX - 1,
                     ECUIO_FRAME_DATA_MAX, ECUIO_VALUE_TEXT));
    {
        ecuio_signal_def_t d =
            text(0x18DAF110, 3, 0xFFFFFF, ECUIO_FRAME_DATA_MAX - 1,
                 ECUIO_FRAME_DATA_MAX);
        d.mux_offset = 255;
        EXPECT_EQ(address(d), "18DAF110:FFFFFF@255:4095:t4096");
        EXPECT_EQ(address(d).size(), 30u);
    }
    EXPECT_EQ(in_frame_text, "18DAF110:FFFFFF:4095:t4096");
    {
        ecuio_signal_def_t d = number(0x1FFFFFFF, 3, 0xFFFFFF,
                                      ECUIO_FRAME_DATA_MAX - 1, 7, 32, 255);
        EXPECT_EQ(address(d), "1FFFFFFF:FFFFFF@255:4095.7:32");
    }
    /* The fields' own most: start_byte is 16 bits */
    {
        ecuio_signal_def_t d =
            text(0x1FFFFFFF, 3, 0xFFFFFF, 65535, ECUIO_FRAME_DATA_MAX);
        d.mux_offset = 255;
        EXPECT_EQ(address(d), "1FFFFFFF:FFFFFF@255:65535:t4096");
        EXPECT_EQ(address(d).size(), size_t{ECUIO_SIGNAL_KEY_MAX});
        ecuio_signal_def_t n =
            number(0xFFFFFFFF, 3, 0xFFFFFF, 65535, 7, 32, 255);
        EXPECT_EQ(address(n), "FFFFFFFF:FFFFFF@255:65535.7:32");
        EXPECT_LE(address(n).size(), size_t{ECUIO_SIGNAL_KEY_MAX});
    }
    /* Only a def the validator refuses has no room: a fourth key byte, a
     * text field longer than a frame */
    {
        ecuio_signal_def_t d =
            text(0x1FFFFFFF, 4, 0xFFFFFFFF, 65535, ECUIO_FRAME_DATA_MAX);
        d.mux_offset = 255;
        EXPECT_NE(ecuio_validate_signal(&d), ECUIO_VALIDATE_OK);
        EXPECT_EQ(address(d, ECUIO_ERR_INVALID_ARGUMENT), "");
        ecuio_signal_def_t e = text(0x1FFFFFFF, 3, 0xFFFFFF, 65535, 65535);
        e.mux_offset = 255;
        EXPECT_NE(ecuio_validate_signal(&e), ECUIO_VALIDATE_OK);
        EXPECT_EQ(address(e, ECUIO_ERR_INVALID_ARGUMENT), "");
    }

    /* Every def of the built-in sets, and the shapes above */
    std::vector<std::string> keys;
    for (const char* profile : {"obd2", "mock"}) {
        const ecuio_signal_set_t* set = nullptr;
        ASSERT_EQ(ecuio_profile_lookup(profile, &set), ECUIO_OK);
        for (uint32_t i = 0; i < set->count; ++i) {
            if ((set->defs[i].flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
                keys.push_back(address(set->defs[i]));
            }
        }
    }
    keys.push_back(in_frame_text);
    keys.push_back(address(number(0x1A0, 0, 0, 0, 4, 12)));
    keys.push_back(address(number(0, 0, 0, 0, 0, 8)));
    ASSERT_GT(keys.size(), 100u);
    for (const std::string& k : keys) {
        ASSERT_FALSE(k.empty());
        const char c = k[0];
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) << k;
        EXPECT_NE(k.find(':'), std::string::npos) << k;
        EXPECT_FALSE(author_key(k)) << k;
        EXPECT_NE(k.compare(0, 3, "id:"), 0) << k;
        EXPECT_EQ(k.find_first_of(", \"\r\n\t"), std::string::npos) << k;
        EXPECT_LE(k.size(), size_t{ECUIO_SIGNAL_KEY_MAX}) << k;
    }
}

/* A built-in set, which has no file: each def under its address, a
 * computed one under its id */
TEST(SignalKeysTest, ABuiltInSetsKeysAreAddressesAndAComputedDefsItsId)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const std::vector<std::string> keys = keys_of(obd2, nullptr);
    ASSERT_EQ(keys.size(), obd2->count);
    std::set<std::string> seen;
    uint32_t computed = 0;
    for (uint32_t i = 0; i < obd2->count; ++i) {
        const ecuio_signal_def_t& d = obd2->defs[i];
        EXPECT_TRUE(seen.insert(keys[i]).second) << keys[i] << " twice";
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            EXPECT_EQ(keys[i], "id:" + std::to_string(d.id));
            ++computed;
        } else {
            EXPECT_EQ(keys[i], address(d)) << d.name;
        }
    }
    EXPECT_GE(computed, 1u) << "obd2's Boost";
    EXPECT_EQ(keys[0], "7E8:4104:2.0:8") << obd2->defs[0].name;
    EXPECT_EQ(seen.count("7E8:410C:2.0:16"), 1u) << "engine speed, PID 0C";

    /* The serial ports' contract: the count alone, and a short array
     * filled and not run past */
    uint32_t count = 0;
    ecuio_signal_key_t two[3];
    std::memset(two, 0x5A, sizeof two);
    EXPECT_EQ(ecuio_signal_set_keys(obd2, nullptr, two, 2, &count), ECUIO_OK);
    EXPECT_EQ(count, obd2->count);
    EXPECT_EQ(std::string(two[0].text), keys[0]);
    EXPECT_EQ(std::string(two[1].text), keys[1]);
    EXPECT_EQ(two[2].text[0], 0x5A);

    count = 77;
    EXPECT_EQ(ecuio_signal_set_keys(nullptr, nullptr, two, 2, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(ecuio_signal_set_keys(obd2, nullptr, two, 2, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    count = 77;
    EXPECT_EQ(ecuio_signal_set_keys(obd2, nullptr, nullptr, 2, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(count, 0u);

    /* A set with no defs has no keys */
    ecuio_signal_set_t none = {};
    none.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    count = 77;
    EXPECT_EQ(ecuio_signal_set_keys(&none, nullptr, nullptr, 0, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u);
}

/* A file's set: the later of two signals at one address and a computed
 * signal go under their author's keys with the file, under their ids
 * without it; a base def keeps its address, and a file's signal at a
 * base def's address takes its author's key */
TEST(SignalKeysTest, AFilesSignalsWithNoAddressOfTheirOwnTakeTheirAuthorsKeys)
{
    const File f = load(
        R"({"havne_signals":2,"name":"Keys","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"},{"id":"bcm","rx":"7E9"}],"signals":[)"
        R"({"key":"gear","name":"Gear","ecu":"pcm","read":{"service":"22","number":"1E12"},"field":{"byte":0,"bits":8}},)"
        R"({"key":"gear_word","name":"Gear position","kind":"enum","ecu":"pcm","read":{"service":"22","number":"1E12"},"field":{"byte":0},"values":{"1":"First","2":"Second"}},)"
        R"({"key":"volts","name":"Battery voltage","ecu":"pcm","read":{"service":"22","number":"01A4"},"field":{"byte":0,"bits":8}},)"
        R"({"key":"bcm_volts","name":"Battery volts","ecu":"bcm","read":{"service":"22","number":"01A4"},"field":{"byte":0,"bits":8}},)"
        R"({"key":"twice","name":"Twice","kind":"computed","expr":"gear * 2"}]})");
    ASSERT_NE(f, nullptr);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    const auto index = [&](const char* key) {
        const ecuio_signal_def_t* d = ecuio_signal_file_find(f.get(), key);
        EXPECT_NE(d, nullptr) << key;
        return static_cast<uint32_t>(d - set->defs);
    };
    const std::vector<std::string> keys = keys_of(set, f.get());
    EXPECT_EQ(keys[index("gear")], "7E8:621E12:3.0:8");
    EXPECT_EQ(keys[index("gear_word")], "gear_word")
        << "the later of two at one address";
    EXPECT_EQ(keys[index("volts")], "7E8:6201A4:3.0:8");
    EXPECT_EQ(keys[index("bcm_volts")], "7E9:6201A4:3.0:8")
        << "another module's is another address";
    EXPECT_EQ(keys[index("twice")], "twice");
    EXPECT_EQ(std::set<std::string>(keys.begin(), keys.end()).size(),
              keys.size());

    /* The set alone: no author to ask */
    const std::vector<std::string> bare = keys_of(set, nullptr);
    EXPECT_EQ(bare[index("gear")], "7E8:621E12:3.0:8");
    EXPECT_EQ(bare[index("gear_word")],
              "id:" + std::to_string(set->defs[index("gear_word")].id));
    EXPECT_EQ(bare[index("twice")],
              "id:" + std::to_string(set->defs[index("twice")].id));

    /* A file of another set */
    uint32_t count = 77;
    ecuio_signal_key_t k;
    EXPECT_EQ(
        ecuio_signal_set_keys(ecuio_obd2_signal_set(), f.get(), &k, 1, &count),
        ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(count, 0u);

    /* A file over obd2: the base defs as in obd2 alone, the file's
     * signal at a base def's address under its author's key */
    const File over = load(
        R"({"havne_signals":2,"name":"Over","source":{"name":"t"},"extends":"obd2","signals":[)"
        R"({"key":"rpm_raw","name":"Engine speed, raw","read":{"service":"01","number":"0C"},"field":{"byte":0,"bits":16,"order":"be"}},)"
        R"({"key":"half","name":"Half","kind":"computed","expr":"rpm_raw / 2"}]})");
    ASSERT_NE(over, nullptr);
    const ecuio_signal_set_t* oset = ecuio_signal_file_set(over.get());
    const std::vector<std::string> okeys = keys_of(oset, over.get());
    const std::vector<std::string> base =
        keys_of(ecuio_obd2_signal_set(), nullptr);
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    uint32_t matched = 0;
    for (uint32_t i = 0; i < oset->count; ++i) {
        for (uint32_t b = 0; b < obd2->count; ++b) {
            if (oset->defs[i].id == obd2->defs[b].id) {
                EXPECT_EQ(okeys[i], base[b]) << obd2->defs[b].name;
                ++matched;
            }
        }
    }
    EXPECT_EQ(matched, obd2->count) << "every base def, under the key it has "
                                       "in obd2";
    const uint32_t raw = static_cast<uint32_t>(
        ecuio_signal_file_find(over.get(), "rpm_raw") - oset->defs);
    EXPECT_EQ(okeys[raw], "rpm_raw") << "obd2's engine speed has the address";
    EXPECT_EQ(address(oset->defs[raw]), "7E8:410C:2.0:16");
    const uint32_t half = static_cast<uint32_t>(
        ecuio_signal_file_find(over.get(), "half") - oset->defs);
    EXPECT_EQ(okeys[half], "half");
}

/* Of two defs at one address the earlier in the set's order keeps the
 * address key, whichever was written first: a signal placed ahead of
 * another at its address takes the key from it */
TEST(SignalKeysTest, TheEarlierOfTwoAtOneAddressKeepsTheAddressKey)
{
    const char* const kGear =
        R"({"key":"gear","name":"Gear","ecu":"pcm","read":{"service":"22","number":"1E12"},"field":{"byte":0,"bits":8}})";
    const char* const kWord =
        R"({"key":"gear_word","name":"Gear position","kind":"enum","ecu":"pcm","read":{"service":"22","number":"1E12"},"field":{"byte":0},"values":{"1":"First"}})";
    const auto keys = [](const char* first, const char* second) {
        const File f = load(
            std::string(
                R"({"havne_signals":1,"name":"Keys","source":{"name":"t"},)"
                R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)") +
            first + "," + second + "]}");
        EXPECT_NE(f, nullptr);
        std::map<std::string, std::string> by_author;
        if (f == nullptr) {
            return by_author;
        }
        const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
        const std::vector<std::string> all = keys_of(set, f.get());
        for (const char* author : {"gear", "gear_word"}) {
            const ecuio_signal_def_t* d =
                ecuio_signal_file_find(f.get(), author);
            EXPECT_NE(d, nullptr) << author;
            if (d != nullptr) {
                by_author[author] = all[static_cast<size_t>(d - set->defs)];
            }
        }
        return by_author;
    };
    const std::map<std::string, std::string> gear_first = keys(kGear, kWord);
    EXPECT_EQ(gear_first.at("gear"), "7E8:621E12:3.0:8");
    EXPECT_EQ(gear_first.at("gear_word"), "gear_word");
    const std::map<std::string, std::string> word_first = keys(kWord, kGear);
    EXPECT_EQ(word_first.at("gear_word"), "7E8:621E12:3.0:8")
        << "placed ahead, it takes the address";
    EXPECT_EQ(word_first.at("gear"), "gear");
}

/* One address has one key in every set, whatever the signal is called
 * there and whatever else the set holds */
TEST(SignalKeysTest, OneAddressHasOneKeyInEverySet)
{
    const File a = load(
        R"({"havne_signals":1,"name":"A","source":{"name":"t"},)"
        R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
        R"({"key":"oil","name":"Oil temperature","ecu":"pcm","read":{"service":"22","number":"1E1C"},"field":{"byte":1,"bits":8},"scale":0.5,"unit":"deg C"}]})");
    const File b = load(
        R"({"havne_signals":1,"name":"B","source":{"name":"t"},)"
        R"("ecus":[{"id":"tcm","rx":"7E9"},{"id":"engine","rx":"7E8"}],"signals":[)"
        R"({"key":"x","name":"X","ecu":"tcm","read":{"service":"22","number":"0001"},"field":{"byte":0,"bits":8}},)"
        R"({"key":"engine_oil_temp","name":"Engine oil temp","ecu":"engine","read":{"service":"22","number":"1E1C"},"field":{"byte":1,"bits":8,"signed":true}}]})");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    const std::vector<std::string> ka =
        keys_of(ecuio_signal_file_set(a.get()), a.get());
    const std::vector<std::string> kb =
        keys_of(ecuio_signal_file_set(b.get()), b.get());
    const uint32_t ia =
        static_cast<uint32_t>(ecuio_signal_file_find(a.get(), "oil") -
                              ecuio_signal_file_set(a.get())->defs);
    const uint32_t ib = static_cast<uint32_t>(
        ecuio_signal_file_find(b.get(), "engine_oil_temp") -
        ecuio_signal_file_set(b.get())->defs);
    EXPECT_EQ(ka[ia], "7E8:621E1C:4.0:8");
    EXPECT_EQ(kb[ib], ka[ia]);
}

/* A person's text names the def with that key, else the one def with
 * that name; a name several have is refused with their keys */
TEST(SignalKeysTest, FindTakesAKeyThenANameOneSignalHas)
{
    std::vector<ecuio_signal_def_t> defs;
    const auto add = [&](uint32_t rx, uint32_t did, const char* name) {
        ecuio_signal_def_t d = number(rx, 3, 0x620000u | did, 3, 0, 8);
        d.id = static_cast<uint16_t>(100 + defs.size());
        std::snprintf(d.name, sizeof d.name, "%s", name);
        defs.push_back(d);
    };
    add(0x7E8, 0x01A4, "Battery voltage");
    add(0x7E9, 0x01A4, "Battery voltage");
    add(0x7E8, 0x0D00, "Speed");
    add(0x7E8, 0x0D01, "7E8:620D00:3.0:8"); /* named as another's key */
    const ecuio_signal_set_t set = set_of(defs);
    ASSERT_EQ(ecuio_validate_signal(&defs[0]), ECUIO_VALIDATE_OK);
    std::vector<ecuio_signal_key_t> keys(defs.size());
    uint32_t count = 0;
    ASSERT_EQ(ecuio_signal_set_keys(&set, nullptr, keys.data(),
                                    static_cast<uint32_t>(keys.size()), &count),
              ECUIO_OK);
    ASSERT_EQ(count, 4u);

    const auto find = [&](const char* text, uint32_t n = 4) {
        uint32_t index = 77;
        const ecuio_status_t st =
            ecuio_signal_set_find(&set, keys.data(), n, text, &index);
        if (st != ECUIO_OK) {
            EXPECT_EQ(st, ECUIO_ERR_INVALID_ARGUMENT) << text;
            EXPECT_EQ(index, 77u) << "untouched on a refusal";
            return UINT32_MAX;
        }
        return index;
    };
    EXPECT_EQ(find("7E8:6201A4:3.0:8"), 0u);
    EXPECT_EQ(find("7E9:6201A4:3.0:8"), 1u);
    EXPECT_EQ(find("Speed"), 2u);
    EXPECT_EQ(find("7E8:620D00:3.0:8"), 2u) << "a key before a name";
    EXPECT_EQ(find("7E8:620D01:3.0:8"), 3u);

    EXPECT_EQ(find("Battery voltage"), UINT32_MAX);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "2 signals are named 'Battery voltage': name one by its key "
                 "(7E8:6201A4:3.0:8, 7E9:6201A4:3.0:8)");
    EXPECT_EQ(find("Boost"), UINT32_MAX);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "no signal with the key or name 'Boost'");
    EXPECT_EQ(find("speed"), UINT32_MAX) << "a name as it is written";
    EXPECT_EQ(find(""), UINT32_MAX);
    EXPECT_STREQ(ecuio_last_error_detail(), "a signal's key or name is empty");
    EXPECT_EQ(find("Battery voltage\nx"), UINT32_MAX);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "no signal with the key or name 'Battery voltage x'")
        << "one line";
    /* A text past the 96 bytes a refusal shows ends before a character
     * the cut would split: 95 bytes, then one of two */
    const std::string long_name = std::string(95, 'a') + "\xC3\xA9" + "tail";
    EXPECT_EQ(find(long_name.c_str()), UINT32_MAX);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "no signal with the key or name '" + std::string(95, 'a') + "'");
    const std::string fits = std::string(94, 'a') + "\xC3\xA9" + "tail";
    EXPECT_EQ(find(fits.c_str()), UINT32_MAX);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "no signal with the key or name '" + std::string(94, 'a') +
                  "\xC3\xA9" + "'")
        << "a character that ends at the cut stays";

    /* Only the defs the keys cover */
    EXPECT_EQ(find("Battery voltage", 1), 0u);
    EXPECT_EQ(find("Speed", 2), UINT32_MAX);
    EXPECT_EQ(find("Speed", 99), 2u) << "no further than the set";

    uint32_t index = 77;
    EXPECT_EQ(ecuio_signal_set_find(nullptr, keys.data(), 4, "Speed", &index),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_set_find(&set, nullptr, 4, "Speed", &index),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_set_find(&set, keys.data(), 4, nullptr, &index),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_set_find(&set, keys.data(), 4, "Speed", nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(index, 77u);

    /* More of one name than the refusal lists */
    std::vector<ecuio_signal_def_t> many;
    for (uint32_t i = 0; i < 11; ++i) {
        ecuio_signal_def_t d = number(0x700 + i, 3, 0x620001, 3, 0, 8);
        d.id = static_cast<uint16_t>(200 + i);
        std::snprintf(d.name, sizeof d.name, "Voltage");
        many.push_back(d);
    }
    const ecuio_signal_set_t mset = set_of(many);
    std::vector<ecuio_signal_key_t> mkeys(many.size());
    ASSERT_EQ(ecuio_signal_set_keys(&mset, nullptr, mkeys.data(), 11, &count),
              ECUIO_OK);
    EXPECT_EQ(ecuio_signal_set_find(&mset, mkeys.data(), 11, "Voltage", &index),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "11 signals are named 'Voltage': name one by its key "
                 "(700:620001:3.0:8, 701:620001:3.0:8, 702:620001:3.0:8, "
                 "703:620001:3.0:8, 704:620001:3.0:8, 705:620001:3.0:8, "
                 "706:620001:3.0:8, 707:620001:3.0:8 and 3 more)");
}

/* A set built by hand with two defs at one address and no file: the
 * later goes by its id, and the keys stay one for each def */
TEST(SignalKeysTest, WithNoFileTheLaterOfTwoAtOneAddressGoesByItsId)
{
    std::vector<ecuio_signal_def_t> defs;
    defs.push_back(number(0x7E8, 3, 0x621E12, 3, 0, 8));
    defs.push_back(number(0x7E8, 3, 0x621E13, 3, 0, 8));
    defs.push_back(number(0x7E8, 3, 0x621E12, 3, 0, 8));
    defs.push_back(number(0x7E8, 3, 0x621E12, 3, 0, 8));
    for (uint32_t i = 0; i < defs.size(); ++i) {
        defs[i].id = static_cast<uint16_t>(40 + i);
    }
    defs[3].kind = ECUIO_VALUE_INT; /* read another way: the same address */
    const ecuio_signal_set_t set = set_of(defs);
    const std::vector<std::string> keys = keys_of(&set, nullptr);
    EXPECT_EQ(keys[0], "7E8:621E12:3.0:8");
    EXPECT_EQ(keys[1], "7E8:621E13:3.0:8");
    EXPECT_EQ(keys[2], "id:42");
    EXPECT_EQ(keys[3], "id:43");
}
