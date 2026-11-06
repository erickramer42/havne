// File/layer: tests/test_obdb_import.cpp | GTest, UNIT tier, Qt-free (test_core)
// Testing: UNIT: ObdbImportTest (test_core)

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ecuio/obdb.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "json.h"        /* core/src */
#include "obdb_import.h" /* core/src: year_takes, field_of */

namespace {

struct Note {
    uint32_t what = 0;
    uint32_t line = 0;
    std::string signal, key, field, text;
};

int collect(const ecuio_obdb_note_t* n, void* user)
{
    auto* notes = static_cast<std::vector<Note>*>(user);
    notes->push_back({n->what, n->line, n->signal, n->key, n->field, n->text});
    return 0;
}

/* An import and, when it succeeded, its file loaded */
struct Imported {
    ecuio_status_t status = ECUIO_OK;
    std::string json;
    std::string detail;
    std::vector<Note> notes;
    ecuio_signal_file_t* file = nullptr;

    Imported() = default;
    Imported(const Imported&) = delete;
    Imported& operator=(const Imported&) = delete;
    ~Imported() { ecuio_signal_file_free(file); }

    const ecuio_signal_set_t& set() const
    {
        return *ecuio_signal_file_set(file);
    }
    const ecuio_signal_def_t* def(const char* key) const
    {
        return ecuio_signal_file_find(file, key);
    }
    /* The first note of a kind about a signal (and a field) */
    const Note* note(const std::string& signal, uint32_t what,
                     const std::string& field = "") const
    {
        for (const Note& n : notes) {
            if (n.signal == signal && n.what == what &&
                (field.empty() || n.field == field)) {
                return &n;
            }
        }
        return nullptr;
    }
    size_t count(uint32_t what) const
    {
        size_t c = 0;
        for (const Note& n : notes) {
            c += n.what == what ? 1u : 0u;
        }
        return c;
    }
};

void import(Imported& out, const std::string& set,
            const ecuio_obdb_options_t* options = nullptr)
{
    char* json = nullptr;
    size_t length = 0;
    out.status = ecuio_obdb_import(set.data(), set.size(), options, collect,
                                   &out.notes, &json, &length);
    out.detail = ecuio_last_error_detail();
    if (out.status != ECUIO_OK) {
        EXPECT_EQ(json, nullptr);
        return;
    }
    ASSERT_NE(json, nullptr);
    out.json.assign(json, length);
    EXPECT_EQ(std::strlen(json), length);
    ecuio_obdb_free(json);
    ASSERT_EQ(
        ecuio_signal_file_load(out.json.data(), out.json.size(), &out.file),
        ECUIO_OK)
        << ecuio_last_error_detail() << "\n"
        << out.json;
}

ecuio_json::Value parsed(const std::string& text)
{
    ecuio_json::Value v;
    ecuio_json::Error e;
    EXPECT_TRUE(ecuio_json::parse(text.data(), text.size(), &v, &e))
        << e.to_string();
    return v;
}

std::string labels_of(const ecuio_signal_set_t& set,
                      const ecuio_signal_def_t& d)
{
    std::string out;
    for (uint32_t i = 0; i < d.table_count; ++i) {
        const ecuio_enum_entry_t& e = set.enums[d.table_first + i];
        out += std::to_string(e.raw) + "=" + e.label + ";";
    }
    return out;
}

/* A command of one signal on the transmission's module */
std::string one(const std::string& extra, const std::string& did,
                const std::string& signal)
{
    return "{\"hdr\":\"7E1\",\"rax\":\"7E9\"" + extra + ",\"cmd\":{\"22\":\"" +
           did + "\"},\"freq\":1,\"signals\":[" + signal + "]}";
}

std::string sig(const std::string& id, const std::string& fmt)
{
    return "{\"id\":\"" + id + "\",\"name\":\"" + id + "\",\"fmt\":" + fmt +
           "}";
}

const char* const kByte = "{\"len\":8,\"max\":255,\"unit\":\"volts\"}";

} // namespace

// A filter takes its years, both ends of a range included, a range with
// from past to its outside, and model year 0 only an unfiltered command
TEST(ObdbImportTest, AModelYearTakesTheCommandsItsFilterNames)
{
    EXPECT_TRUE(ecuio_obdb::year_takes(nullptr, 0));
    EXPECT_TRUE(ecuio_obdb::year_takes(nullptr, 2020));
    const ecuio_json::Value range = parsed("{\"from\":2018,\"to\":2020}");
    EXPECT_FALSE(ecuio_obdb::year_takes(&range, 0)) << "no year, no filter met";
    EXPECT_FALSE(ecuio_obdb::year_takes(&range, 2017));
    EXPECT_TRUE(ecuio_obdb::year_takes(&range, 2018));
    EXPECT_TRUE(ecuio_obdb::year_takes(&range, 2020));
    EXPECT_FALSE(ecuio_obdb::year_takes(&range, 2021));
    const ecuio_json::Value wrap = parsed("{\"from\":2020,\"to\":2010}");
    EXPECT_TRUE(ecuio_obdb::year_takes(&wrap, 2021));
    EXPECT_TRUE(ecuio_obdb::year_takes(&wrap, 2009));
    EXPECT_FALSE(ecuio_obdb::year_takes(&wrap, 2015));
    const ecuio_json::Value from = parsed("{\"from\":2018}");
    EXPECT_TRUE(ecuio_obdb::year_takes(&from, 2030));
    EXPECT_FALSE(ecuio_obdb::year_takes(&from, 2017));
    const ecuio_json::Value to = parsed("{\"to\":2018}");
    EXPECT_TRUE(ecuio_obdb::year_takes(&to, 2001));
    EXPECT_FALSE(ecuio_obdb::year_takes(&to, 2019));
    // Year 0 is no year, never one before them all
    EXPECT_FALSE(ecuio_obdb::year_takes(&to, 0));
    EXPECT_FALSE(ecuio_obdb::year_takes(&wrap, 0));
    const ecuio_json::Value years = parsed("{\"years\":[2012,2015]}");
    EXPECT_TRUE(ecuio_obdb::year_takes(&years, 2015));
    EXPECT_FALSE(ecuio_obdb::year_takes(&years, 2013));
    const ecuio_json::Value none = parsed("{}");
    EXPECT_FALSE(ecuio_obdb::year_takes(&none, 2015));
}

// OBDb's bit index counts from the top bit of the first byte after the
// echo: whole bytes are big-endian (little under blsb, past 8 bits), a
// field within a byte little-endian from that byte's lowest bit, and a
// field across bytes that isn't whole bytes has no signal file form
TEST(ObdbImportTest, AFieldIsWhereOBDbsDecoderReadsIt)
{
    ecuio_obdb::field f;
    ASSERT_TRUE(ecuio_obdb::field_of(16, 16, false, &f));
    EXPECT_EQ(f.byte, 2u);
    EXPECT_EQ(f.bits, 16u);
    EXPECT_FALSE(f.little_endian);
    ASSERT_TRUE(ecuio_obdb::field_of(16, 16, true, &f));
    EXPECT_TRUE(f.little_endian);
    ASSERT_TRUE(ecuio_obdb::field_of(8, 8, true, &f));
    EXPECT_FALSE(f.little_endian) << "OBDb applies blsb past 8 bits only";
    ASSERT_TRUE(ecuio_obdb::field_of(0, 1, false, &f));
    EXPECT_EQ(f.byte, 0u);
    EXPECT_EQ(f.bit, 7u) << "bit index 0 is the byte's top bit";
    EXPECT_TRUE(f.little_endian);
    ASSERT_TRUE(ecuio_obdb::field_of(20, 3, false, &f));
    EXPECT_EQ(f.byte, 2u);
    EXPECT_EQ(f.bit, 1u);
    EXPECT_EQ(f.bits, 3u);
    ASSERT_TRUE(ecuio_obdb::field_of(4, 4, false, &f));
    EXPECT_EQ(f.bit, 0u) << "the low nibble";
    EXPECT_FALSE(ecuio_obdb::field_of(4, 8, false, &f));
    EXPECT_FALSE(ecuio_obdb::field_of(8, 12, true, &f));
    EXPECT_FALSE(ecuio_obdb::field_of(0, 0, false, &f));
    EXPECT_FALSE(ecuio_obdb::field_of(0, 33, false, &f));
}

// A set of numbers, an enumeration and a bit field converts to a file
// that loads, each def where OBDb reads it, the scale mul / div, the
// unit the built-in sets spell, and each difference a note
TEST(ObdbImportTest, ASetConvertsToAFileThatLoads)
{
    const std::string set =
        "{\"ecu\":[{\"hdr\":\"7E1\",\"type\":\"transmission\"}],\n"
        "\"commands\":[\n" +
        one("", "1E1C",
            "{\"id\":\"TCM_TFT\",\"name\":\"Transmission fluid temp\","
            "\"path\":\"Transmission\",\"description\":\"Sump\",\"fmt\":"
            "{\"len\":16,\"max\":215,\"min\":-40,\"add\":-40,\"div\":4,"
            "\"unit\":\"celsius\"}},\n"
            "{\"id\":\"TCM_GEAR\",\"name\":\"Gear\",\"fmt\":{\"bix\":16,"
            "\"len\":4,\"map\":{\"0\":{\"description\":\"Park\",\"value\":"
            "\"P\"},"
            "\"1\":\"Reverse\",\"9\":{\"value\":\"NINE\"},\"01\":\"Again\","
            "\"16\":\"Wide\"}}},\n"
            "{\"id\":\"TCM_FLAGS\",\"name\":\"=Flags\",\"fmt\":{\"bix\":20,"
            "\"len\":3,\"max\":7,\"unit\":\"scalar\"}}") +
        ",\n{\"hdr\":\"7E0\",\"rax\":\"7E8\",\"cmd\":{\"01\":\"0D\"},\"freq\":"
        "1,"
        "\"signals\":[{\"id\":\"SPEED\",\"name\":\"Speed\",\"path\":"
        "\"Movement\","
        "\"fmt\":{\"len\":8,\"max\":255,\"unit\":\"kilometersPerHour\","
        "\"nullmax\":255}}]}]}";
    Imported im;
    import(im, set);
    ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
    EXPECT_EQ(im.count(ECUIO_OBDB_NOTE_CONVERTED), 4u);
    EXPECT_EQ(im.count(ECUIO_OBDB_NOTE_SKIPPED), 0u);
    EXPECT_EQ(im.set().count, ecuio_obd2_signal_set()->count + 4)
        << "extends obd2 by default";

    const ecuio_signal_def_t* tft = im.def("tcm_tft");
    ASSERT_NE(tft, nullptr);
    EXPECT_EQ(tft->message_id, 0x7E9u);
    EXPECT_EQ(tft->mux_key, 0x621E1Cu);
    EXPECT_EQ(tft->start_byte, 3u) << "the field after 62 1E 1C";
    EXPECT_EQ(tft->bit_length, 16u);
    EXPECT_EQ(tft->byte_order, ECUIO_BYTE_ORDER_BE);
    EXPECT_DOUBLE_EQ(tft->scale, 0.25);
    EXPECT_DOUBLE_EQ(tft->offset, -40);
    EXPECT_STREQ(tft->unit, "\xC2\xB0"
                            "C");
    EXPECT_EQ(tft->decimal_places, 2u);
    EXPECT_EQ(tft->group, ECUIO_SIGNAL_GROUP_VEHICLE);
    EXPECT_STREQ(tft->name, "Transmission fluid temp");
    const Note* range = im.note("TCM_TFT", ECUIO_OBDB_NOTE_CHANGED, "max");
    ASSERT_NE(range, nullptr);
    EXPECT_EQ(range->text, "OBDb holds the value within -40 to 215; Havne "
                           "shows it as read");
    EXPECT_EQ(range->key, "tcm_tft");
    EXPECT_EQ(range->line, 3u) << "the signal's line";

    const ecuio_signal_def_t* gear = im.def("tcm_gear");
    ASSERT_NE(gear, nullptr);
    EXPECT_EQ(gear->kind, ECUIO_VALUE_ENUM);
    EXPECT_EQ(gear->start_byte, 5u);
    EXPECT_EQ(gear->byte_order, ECUIO_BYTE_ORDER_LE);
    EXPECT_EQ(gear->bit_offset, 4u) << "the top nibble";
    EXPECT_EQ(gear->bit_length, 4u);
    EXPECT_EQ(labels_of(im.set(), *gear), "0=Park;1=Reverse;9=NINE;")
        << "a description first, else the value";
    const Note* again = im.note("TCM_GEAR", ECUIO_OBDB_NOTE_CHANGED, "map");
    ASSERT_NE(again, nullptr);
    EXPECT_NE(again->text.find("'01'"), std::string::npos) << again->text;
    bool wide = false;
    for (const Note& n : im.notes) {
        wide = wide || n.text == "raw 16 can't occur in 4 bits: left out";
    }
    EXPECT_TRUE(wide);

    const ecuio_signal_def_t* flags = im.def("tcm_flags");
    ASSERT_NE(flags, nullptr);
    EXPECT_STREQ(flags->name, "Flags");
    EXPECT_EQ(flags->bit_offset, 1u);
    EXPECT_EQ(flags->bit_length, 3u);
    EXPECT_STREQ(flags->unit, "");
    EXPECT_NE(im.note("TCM_FLAGS", ECUIO_OBDB_NOTE_CHANGED, "name"), nullptr);

    const ecuio_signal_def_t* speed = im.def("speed");
    ASSERT_NE(speed, nullptr);
    EXPECT_EQ(speed->message_id, 0x7E8u) << "obd2's module, shared";
    EXPECT_EQ(speed->start_byte, 2u);
    EXPECT_STREQ(speed->unit, "km/h");
    EXPECT_NE(im.note("SPEED", ECUIO_OBDB_NOTE_CHANGED, "nullmax"), nullptr);

    EXPECT_NE(
        im.json.find("\"id\":\"transmission\",\"rx\":\"7E9\",\"tx\":\"7E1\""),
        std::string::npos)
        << "the set's ecu list names the module\n"
        << im.json;
    EXPECT_NE(im.json.find("\"name\": \"OBDb editors and contributors\""),
              std::string::npos);
    EXPECT_NE(im.json.find("\"license\": \"CC-BY-SA-4.0\""), std::string::npos);
    EXPECT_NE(im.json.find("\"modified\": \"Converted to Havne's signal-file "
                           "format\""),
              std::string::npos);
    EXPECT_NE(im.json.find("\"notes\":\"Sump\""), std::string::npos);
    EXPECT_STREQ(ecuio_signal_file_profile(im.file), "user.obdb-import");
    EXPECT_NE(im.note("", ECUIO_OBDB_NOTE_FILE, "freq"), nullptr);
}

// Every command or signal a signal file can't say is left out with why,
// and the rest still convert
TEST(ObdbImportTest, WhatASignalFileCantSayIsLeftOutWithWhy)
{
    const std::string byte = kByte;
    const std::string set =
        "{\"commands\":[" +
        std::string("{\"hdr\":\"7E1\",\"rax\":\"7E9\",\"cmd\":{\"21\":\"01\"},"
                    "\"freq\":1,\"signals\":[") +
        sig("S21", byte) + "]}," +
        "{\"hdr\":\"DA10\",\"rax\":\"10\",\"cmd\":{\"22\":\"0101\"},\"freq\":1,"
        "\"signals\":[" +
        sig("S29", byte) + "]}," +
        one(",\"proto\":\"9141-2\"", "0102", sig("SK", byte)) + "," +
        "{\"hdr\":\"6F1\",\"rax\":\"607\",\"eax\":\"07\",\"cmd\":{\"22\":"
        "\"0103\"},"
        "\"freq\":1,\"signals\":[" +
        sig("SE", byte) + "]}," +
        "{\"hdr\":\"7DF\",\"cmd\":{\"01\":\"0C\"},\"freq\":1,\"signals\":[" +
        sig("SF", byte) + "]}," +
        "{\"hdr\":\"7C0\",\"cmd\":{\"22\":\"0104\"},\"freq\":1,\"signals\":[" +
        sig("SR", byte) + "]}," +
        one(",\"filter\":{\"from\":2018}", "0105", sig("SY", byte)) + "," +
        one("", "0106",
            "{\"id\":\"SH\",\"name\":\"h\",\"hidden\":true,\"fmt\":" + byte +
                "}," + sig("SW", "{\"len\":40,\"max\":1,\"unit\":\"volts\"}") +
                "," +
                sig("SX",
                    "{\"bix\":4,\"len\":8,\"max\":1,\"unit\":\"volts\"}") +
                "," +
                sig("SZ",
                    "{\"len\":8,\"mul\":0,\"max\":1,\"unit\":\"volts\"}") +
                "," +
                sig("SB",
                    "{\"bix\":32760,\"len\":8,\"max\":1,\"unit\":\"volts\"}") +
                "," + sig("OK1", byte)) +
        "]}";
    Imported im;
    import(im, set);
    ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
    const struct {
        const char* signal;
        const char* why;
    } skipped[] = {
        {"S21", "Service 21 isn't one Havne reads"},
        {"S29", "29-bit IDs aren't imported"},
        {"SK",
         "its command is on 9141-2, a K-line protocol Havne doesn't read"},
        {"SE", "extended addressing (eax) isn't supported"},
        {"SF", "a functional request (7DF) has no one module's reply"},
        {"SR",
         "no reply ID (\"rax\"), and none the standard gives request ID 7C0"},
        {"SY", "its command is for some model years only: give the vehicle's "
               "model year"},
        {"SH", "hidden in OBDb"},
        {"SW", "a field of 40 bits: a signal file reads 1 to 32"},
        {"SX", "a field across bytes that isn't whole bytes (bix 4, len 8), "
               "which a signal file can't say"},
        {"SZ", "a scale of 0 or none (mul 0, div 1)"},
        {"SB", "a field past the 4096 bytes a reply holds"},
    };
    for (const auto& s : skipped) {
        SCOPED_TRACE(s.signal);
        const Note* n = im.note(s.signal, ECUIO_OBDB_NOTE_SKIPPED);
        ASSERT_NE(n, nullptr);
        EXPECT_EQ(n->text, s.why);
        EXPECT_EQ(n->key, "");
        EXPECT_GT(n->line, 0u);
    }
    EXPECT_EQ(im.count(ECUIO_OBDB_NOTE_CONVERTED), 1u);
    EXPECT_NE(im.def("ok1"), nullptr);

    // The vehicle's model year takes the filtered command
    ecuio_obdb_options_t o{};
    o.model_year = 2019;
    Imported dated;
    import(dated, set, &o);
    ASSERT_EQ(dated.status, ECUIO_OK);
    EXPECT_NE(dated.def("sy"), nullptr);
    o.model_year = 2017;
    Imported older;
    import(older, set, &o);
    const Note* n = older.note("SY", ECUIO_OBDB_NOTE_SKIPPED);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->text, "its command isn't for model year 2017");
}

// A reply ID the set leaves out is the standard's where there is one,
// and said so
TEST(ObdbImportTest, AMissingReplyIdIsTheStandardsAndNoted)
{
    Imported im;
    import(im, "{\"commands\":[{\"hdr\":\"7E2\",\"cmd\":{\"22\":\"0101\"},"
               "\"freq\":1,\"signals\":[" +
                   sig("A", kByte) + "]}]}");
    ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
    ASSERT_NE(im.def("a"), nullptr);
    EXPECT_EQ(im.def("a")->message_id, 0x7EAu);
    const Note* n = im.note("A", ECUIO_OBDB_NOTE_CHANGED, "rax");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->text, "no reply ID given: 7EA, the one ISO 15765-4 gives 7E2 "
                       "(OBDb takes any module's)");
}

// A session asks at most four modules, obd2's counted, and its requests,
// obd2's counted: past either, the rest are left out and the file loads
TEST(ObdbImportTest, ModulesAndRequestsStopAtWhatASessionAsks)
{
    // Requests 7E1 to 7E5, replies 7E9 to 7ED: obd2's 7E8 and three fit
    std::string fixed = "{\"commands\":[";
    for (int i = 1; i <= 5; ++i) {
        char ids[64];
        std::snprintf(
            ids, sizeof ids,
            "{\"hdr\":\"%03X\",\"rax\":\"%03X\",\"cmd\":{\"22\":\"%04X\"}",
            0x7E0 + i, 0x7E8 + i, 0x0100 + i);
        fixed += std::string(i > 1 ? "," : "") + ids +
                 ",\"freq\":1,\"signals\":[" +
                 sig("M" + std::to_string(i), kByte) + "]}";
    }
    fixed += "]}";
    Imported im;
    import(im, fixed);
    ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
    EXPECT_EQ(im.set().channel_count, 4u);
    EXPECT_EQ(im.count(ECUIO_OBDB_NOTE_CONVERTED), 3u);
    const Note* fourth = im.note("M4", ECUIO_OBDB_NOTE_SKIPPED);
    ASSERT_NE(fourth, nullptr);
    EXPECT_EQ(fourth->text,
              "more modules than a session asks (4, obd2's counted)");

    ecuio_obdb_options_t alone{};
    alone.flags = ECUIO_OBDB_ALONE;
    Imported own;
    import(own, fixed, &alone);
    ASSERT_EQ(own.status, ECUIO_OK);
    EXPECT_EQ(own.count(ECUIO_OBDB_NOTE_CONVERTED), 4u) << "no obd2 to count";
    EXPECT_EQ(own.set().count, 4u);
    EXPECT_EQ(own.json.find("\"extends\""), std::string::npos);

    std::string many = "{\"commands\":[";
    for (int i = 0; i < 300; ++i) {
        char did[8];
        std::snprintf(did, sizeof did, "%04X", 0x2000 + i);
        many += std::string(i > 0 ? "," : "") +
                one("", did, sig("D" + std::to_string(i), kByte));
    }
    many += "]}";
    Imported full;
    import(full, many);
    ASSERT_EQ(full.status, ECUIO_OK) << full.detail;
    const size_t converted = full.count(ECUIO_OBDB_NOTE_CONVERTED);
    EXPECT_LT(converted, 256u);
    EXPECT_GT(converted, 100u);
    EXPECT_EQ(converted + full.count(ECUIO_OBDB_NOTE_SKIPPED), 300u);
    const Note* past = full.note("D299", ECUIO_OBDB_NOTE_SKIPPED);
    ASSERT_NE(past, nullptr);
    EXPECT_EQ(past->text,
              "past the 256 requests a session asks, obd2's counted");
}

// Input that isn't an OBDb set, or holds nothing Havne reads, is refused
// with where and why; a callback's nonzero return cancels
TEST(ObdbImportTest, WhatIsntASetIsRefusedAndACallbackCancels)
{
    Imported list;
    import(list, "[]");
    EXPECT_EQ(list.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(list.detail,
              "line 1, column 1: this isn't an OBDb signal set: it "
              "has no \"commands\" list");
    Imported broken;
    import(broken, "{\"commands\": [");
    EXPECT_EQ(broken.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(broken.detail.compare(0, 7, "line 1,"), 0) << broken.detail;
    Imported empty;
    import(empty, "{\"commands\":[]}");
    EXPECT_EQ(empty.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(empty.detail, "no signal in the set is one Havne can read");

    const std::string set =
        "{\"commands\":[" + one("", "0101", sig("A", kByte)) + "]}";
    char* json = reinterpret_cast<char*>(1);
    size_t length = 1;
    int calls = 0;
    auto stop = [](const ecuio_obdb_note_t*, void* user) {
        ++*static_cast<int*>(user);
        return 1;
    };
    EXPECT_EQ(ecuio_obdb_import(set.data(), set.size(), nullptr, stop, &calls,
                                &json, &length),
              ECUIO_ERR_CANCELED);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(json, nullptr);
    EXPECT_EQ(length, 0u);

    EXPECT_EQ(ecuio_obdb_import(set.data(), set.size(), nullptr, nullptr,
                                nullptr, nullptr, &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_obdb_import(nullptr, 3, nullptr, nullptr, nullptr, &json,
                                &length),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_obdb_options_t bad{};
    bad.flags = 0x80;
    EXPECT_EQ(ecuio_obdb_import(set.data(), set.size(), &bad, nullptr, nullptr,
                                &json, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    bad.flags = 0;
    bad.reserved[2] = 1;
    EXPECT_EQ(ecuio_obdb_import(set.data(), set.size(), &bad, nullptr, nullptr,
                                &json, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    char* none = nullptr;
    EXPECT_EQ(ecuio_obdb_import(set.data(), set.size(), nullptr, nullptr,
                                nullptr, &none, &length),
              ECUIO_OK)
        << "no callback is fine";
    ecuio_obdb_free(none);
    ecuio_obdb_free(nullptr);
}

// The caller's name, URL and commit go in the file; keys come from OBDb's
// ids, camel case split, a letter first, each once
TEST(ObdbImportTest, TheCallersSourceAndKeysFromTheIds)
{
    const std::string set =
        "{\"commands\":[" +
        one("", "0101",
            sig("engineCoolantTemp", kByte) + "," + sig("2WD", kByte) + "," +
                sig("X", kByte) + "," + sig("X", kByte)) +
        "]}";
    ecuio_obdb_options_t o{};
    o.name = "Weekend car";
    o.url = "https://example.org/obdb/Example-Car";
    o.commit = "0123abc";
    Imported im;
    import(im, set, &o);
    ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
    EXPECT_NE(im.def("engine_coolant_temp"), nullptr);
    EXPECT_NE(im.def("s_2wd"), nullptr);
    EXPECT_NE(im.def("x"), nullptr);
    EXPECT_NE(im.def("x_2"), nullptr);
    EXPECT_STREQ(ecuio_signal_file_profile(im.file), "user.weekend-car");
    EXPECT_NE(im.json.find("\"url\": \"https://example.org/obdb/Example-Car\""),
              std::string::npos);
    EXPECT_NE(im.json.find("\"commit\": \"0123abc\""), std::string::npos);
}

// A synthetic ratio of two converted numbers is a computed signal; any
// other formula is left out
TEST(ObdbImportTest, ARatioIsAComputedSignal)
{
    const std::string set =
        "{\"commands\":[" +
        one("", "0101", sig("A", kByte) + "," + sig("B", kByte)) +
        "],\"synthetics\":["
        "{\"id\":\"A_PER_B\",\"name\":\"A per "
        "B\",\"path\":\"Engine\",\"max\":10,"
        "\"unit\":\"scalar\",\"formula\":{\"op\":\"ratio\",\"a\":\"A\",\"b\":"
        "\"B\"}},"
        "{\"id\":\"PICK\",\"name\":\"Pick\",\"path\":\"Engine\",\"max\":1,"
        "\"unit\":\"scalar\",\"formula\":{\"op\":\"select\"}}],"
        "\"signalGroups\":[{\"id\":\"g\",\"matchingRegex\":\"x\"}]}";
    Imported im;
    import(im, set);
    ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
    const ecuio_signal_def_t* ratio = im.def("a_per_b");
    ASSERT_NE(ratio, nullptr);
    EXPECT_NE(ratio->flags & ECUIO_DEF_FLAG_COMPUTED, 0u);
    EXPECT_EQ(ratio->group, ECUIO_SIGNAL_GROUP_ENGINE);
    const ecuio_computed_t& p = im.set().computed[ratio->table_first];
    ASSERT_EQ(p.input_count, 2u);
    EXPECT_EQ(p.input_ids[0], im.def("a")->id);
    EXPECT_EQ(p.input_ids[1], im.def("b")->id);
    ASSERT_GT(p.step_count, 0u);
    EXPECT_EQ(p.steps[p.step_count - 1], ECUIO_COMPUTED_DIVIDE) << "a over b";
    const Note* pick = im.note("PICK", ECUIO_OBDB_NOTE_SKIPPED);
    ASSERT_NE(pick, nullptr);
    EXPECT_EQ(pick->text, "a synthetic's 'select' formula isn't one a signal "
                          "file computes");
    EXPECT_NE(im.note("", ECUIO_OBDB_NOTE_FILE, "signalGroups"), nullptr);
}

// A note is one line whatever the set holds: an id or a protocol quoted
// from it with a line break in it has the break as a space
TEST(ObdbImportTest, ANoteIsOneLineWhateverTheSetHolds)
{
    const std::string set =
        "{\"commands\":[" + one("", "0101", sig("A\\rB", kByte)) + "," +
        one(",\"proto\":\"x\\ny\"", "0102", sig("C", kByte)) + "]}";
    Imported im;
    import(im, set);
    ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
    ASSERT_FALSE(im.notes.empty());
    for (const Note& n : im.notes) {
        for (const std::string* s : {&n.signal, &n.key, &n.field, &n.text}) {
            EXPECT_EQ(s->find_first_of("\r\n"), std::string::npos) << *s;
        }
    }
    EXPECT_NE(im.note("A B", ECUIO_OBDB_NOTE_CONVERTED), nullptr);
    const Note* k = im.note("C", ECUIO_OBDB_NOTE_SKIPPED);
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->text,
              "its command is on x y, a K-line protocol Havne doesn't read");
}

// A request or reply ID of 000 is no module's: that command alone is left
// out, with why
TEST(ObdbImportTest, ARequestOrReplyIdOf0LeavesOutItsCommandAlone)
{
    for (const char* ids :
         {R"("hdr":"000","rax":"7E9")", R"("hdr":"7E1","rax":"000")"}) {
        const std::string bad = std::string("{") + ids +
                                R"(,"cmd":{"22":"1234"},"freq":1,"signals":[)" +
                                sig("BAD", kByte) + "]}";
        Imported im;
        import(im, "{\"commands\":[" + bad + "," +
                       one("", "1235", sig("GOOD", kByte)) + "]}");
        ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
        EXPECT_NE(im.def("good"), nullptr);
        EXPECT_EQ(im.def("bad"), nullptr);
        EXPECT_NE(im.note("BAD", ECUIO_OBDB_NOTE_SKIPPED), nullptr) << ids;
    }
}

// Every formula start goes, spaces between them too, from a name and a
// label alike
TEST(ObdbImportTest, AFormulaStartAfterASpaceGoesToo)
{
    Imported im;
    import(
        im,
        "{\"commands\":[" +
            one("", "1235",
                R"({"id":"V5","name":"+ +5V ref","fmt":{"len":8,"max":255,"unit":"volts"}},)"
                R"({"id":"MODE","name":"Mode","fmt":{"len":8,"map":{"1":"= =y","2":"- @z"}}})") +
            "]}");
    ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
    const ecuio_signal_def_t* v5 = im.def("v5");
    ASSERT_NE(v5, nullptr);
    EXPECT_STREQ(v5->name, "5V ref");
    const ecuio_signal_def_t* mode = im.def("mode");
    ASSERT_NE(mode, nullptr);
    EXPECT_EQ(labels_of(im.set(), *mode), "1=y;2=z;");
}

// A signed field converts signed
TEST(ObdbImportTest, ASignedFieldConvertsSigned)
{
    Imported im;
    import(im,
           "{\"commands\":[" +
               one("", "1235",
                   sig("S16", R"({"len":16,"sign":true,"unit":"scalar"})")) +
               "]}");
    ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
    const ecuio_signal_def_t* s = im.def("s16");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->kind, ECUIO_VALUE_INT);
    EXPECT_NE(im.json.find("\"signed\":true"), std::string::npos) << im.json;
}

// A character a line hides, or one that moves the text around it, is a
// space in a converted name
TEST(ObdbImportTest, AHiddenCharacterInANameIsASpace)
{
    Imported im;
    import(
        im,
        "{\"commands\":[" +
            one("", "1235",
                R"({"id":"H","name":"A\u202EB\u0085C","fmt":{"len":8,"max":255,"unit":"volts"}})") +
            "]}");
    ASSERT_EQ(im.status, ECUIO_OK) << im.detail;
    const ecuio_signal_def_t* h = im.def("h");
    ASSERT_NE(h, nullptr);
    EXPECT_STREQ(h->name, "A B C");
}
