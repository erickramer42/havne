// File/layer: tests/test_signal_file_format3.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileFormat3Test (test_signals)
//
// Signal-file format 3 (ADR-0065 section 9): a name that repeats, a
// signal named as a base def read beside it, one address for one signal;
// an ECU's name, which any format takes, and the module getter.

#include "test_signal_file_support.h"

namespace {

/* A file of the given format around its signals' JSON; head is any
 * member more, ending in a comma */
std::string
file_of(unsigned format, const std::string& signals,
        const std::string& head = R"("ecus":[{"id":"pcm","rx":"7E8"}],)")
{
    return "{\"havne_signals\":" + std::to_string(format) +
           R"(,"name":"T","source":{"name":"t"},)" + head + "\"signals\":[" +
           signals + "]}";
}

/* A number signal read by DID, one byte at the given place */
std::string did(const std::string& key, const std::string& name,
                const std::string& number, unsigned byte = 0,
                const std::string& more = "")
{
    return "{\"key\":\"" + key + "\",\"name\":\"" + name +
           "\",\"read\":{\"service\":\"22\",\"number\":\"" + number +
           "\"},\"field\":{\"byte\":" + std::to_string(byte) + more + "}}";
}

const char kExtends[] = R"("extends":"obd2",)";

ecuio_signal_module_t module_at(const ecuio_signal_file_t* f, uint32_t index,
                                ecuio_status_t* status = nullptr)
{
    ecuio_signal_module_t m{};
    m.struct_size = sizeof m;
    const ecuio_status_t s = ecuio_signal_file_module(f, index, &m);
    if (status != nullptr) {
        *status = s;
    } else {
        EXPECT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    }
    return m;
}

} // namespace

TEST(SignalFileFormat3Test, AFormat3FileLoads)
{
    File f = load(file_of(3, did("a", "A", "D100")));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_format(f.get()), 3u);
    EXPECT_EQ(ECUIO_SIGNAL_FILE_FORMAT, 3u);
    const ecuio_signal_def_t* a = ecuio_signal_file_find(f.get(), "a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->id, ECUIO_USER_SIG_ID_BASE);

    /* Format 2's grammar, unchanged */
    File g = load(file_of(
        3,
        did("a", "A", "D100") +
            R"j(,{"key":"c","name":"C","kind":"computed","expr":"max(a, 3)"})j"));
    ASSERT_NE(g, nullptr) << ecuio_last_error_detail();
    EXPECT_NE(ecuio_signal_file_find(g.get(), "c"), nullptr);
}

TEST(SignalFileFormat3Test, ARefusedFormatNamesTheFormatsRead)
{
    EXPECT_NE(refusal(file_of(4, did("a", "A", "D100")))
                  .find("\"havne_signals\" is format 4, and this Havne reads "
                        "formats 1 to 3"),
              std::string::npos)
        << ecuio_last_error_detail();
}

TEST(SignalFileFormat3Test, TwoSignalsOfOneNameBothLoad)
{
    const std::string two = did("a", "Battery voltage", "D100") + "," +
                            did("b", "Battery voltage", "D101");
    File f = load(file_of(3, two));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_def_t* a = ecuio_signal_file_find(f.get(), "a");
    const ecuio_signal_def_t* b = ecuio_signal_file_find(f.get(), "b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
    EXPECT_STREQ(a->name, "Battery voltage");
    EXPECT_STREQ(b->name, "Battery voltage");

    /* A format 1 or 2 file still names each signal once */
    for (unsigned format : {1u, 2u}) {
        EXPECT_NE(refusal(file_of(format, two))
                      .find("signal 'b' has the name 'Battery voltage', which "
                            "signal 'a' has too: a name names one signal"),
                  std::string::npos)
            << format << ": " << ecuio_last_error_detail();
    }
}

TEST(SignalFileFormat3Test, ASignalNamedAsABaseDefIsASecondSignal)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const std::string map = did("map_kpa", "Intake Manifold Pressure", "D100");
    File f = load(file_of(3, map, kExtends));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    ASSERT_EQ(set->count, obd2->count + 1) << "beside the base def, not for it";
    const ecuio_signal_def_t* own = ecuio_signal_file_find(f.get(), "map_kpa");
    ASSERT_NE(own, nullptr);
    EXPECT_EQ(own->id, ECUIO_USER_SIG_ID_BASE) << "an ID of its own";

    /* Every base def stands as it is: its ID, its address, its program */
    ASSERT_EQ(set->computed_count, obd2->computed_count);
    for (uint32_t i = 0; i < obd2->count; ++i) {
        EXPECT_EQ(
            std::memcmp(&set->defs[i], &obd2->defs[i], sizeof set->defs[i]), 0)
            << obd2->defs[i].name;
    }
    for (uint32_t i = 0; i < obd2->computed_count; ++i) {
        EXPECT_EQ(std::memcmp(&set->computed[i], &obd2->computed[i],
                              sizeof set->computed[i]),
                  0);
    }
    int named = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        named +=
            std::strcmp(set->defs[i].name, "Intake Manifold Pressure") == 0;
    }
    EXPECT_EQ(named, 2);

    /* The same file in format 2 replaces the base def and takes its ID */
    File g = load(file_of(2, map, kExtends));
    ASSERT_NE(g, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_set(g.get())->count, obd2->count);
    EXPECT_EQ(ecuio_signal_file_find(g.get(), "map_kpa")->id,
              ECUIO_OBD2_PID_SIG_ID(0x0B, 0));
}

TEST(SignalFileFormat3Test, OneAddressHasOneSignal)
{
    /* A number and an enum over one byte: formats 1 and 2 take both */
    const std::string pair =
        did("a", "A", "D100") +
        R"(,{"key":"b","name":"B","kind":"enum","read":{"service":"22",)"
        R"("number":"D100"},"field":{"byte":0},"values":{"1":"On"}})";
    EXPECT_NE(load(file_of(2, pair)), nullptr) << ecuio_last_error_detail();
    EXPECT_NE(refusal(file_of(3, pair))
                  .find("signal 'b' reads the address signal 'a' reads "
                        "(7E8:62D100:3.0:8): a format 3 file gives one address "
                        "to one signal"),
              std::string::npos)
        << ecuio_last_error_detail();

    /* Two text fields over the same bytes are one address too */
    const auto text = [](const char* key) {
        return std::string("{\"key\":\"") + key + "\",\"name\":\"N" + key +
               R"(","kind":"text","read":{"service":"22","number":"F190"},)"
               R"("field":{"byte":0,"bytes":17}})";
    };
    EXPECT_NE(refusal(file_of(3, text("v") + "," + text("w")))
                  .find("(7E8:62F190:3:t17)"),
              std::string::npos)
        << ecuio_last_error_detail();

    /* Another width, another byte or another module is another address */
    EXPECT_NE(load(file_of(3, did("a", "A", "D100") + "," +
                                  did("b", "B", "D100", 0, ",\"bits\":16"))),
              nullptr)
        << ecuio_last_error_detail();
    EXPECT_NE(load(file_of(3, did("a", "A", "D100") + "," +
                                  did("b", "B", "D100", 1))),
              nullptr)
        << ecuio_last_error_detail();

    /* A signal at a base def's address is kept beside it, as before */
    const std::string rpm =
        R"({"key":"rpm_raw","name":"Raw speed","read":{"service":"01",)"
        R"("number":"0C"},"field":{"byte":0,"bits":16}})";
    File f = load(file_of(3, rpm, kExtends));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_set(f.get())->count,
              ecuio_obd2_signal_set()->count + 1);
}

TEST(SignalFileFormat3Test, AnEcusNameIsReadInAnyFormat)
{
    const auto named = [](unsigned format, const std::string& name) {
        return file_of(format, did("a", "A", "D100"),
                       R"("ecus":[{"id":"pcm","rx":"7E8","name":)" + name +
                           "}],");
    };
    for (unsigned format : {1u, 2u, 3u}) {
        File f = load(named(format, "\"Engine\""));
        ASSERT_NE(f, nullptr) << format << ": " << ecuio_last_error_detail();
        EXPECT_STREQ(module_at(f.get(), 0).name, "Engine") << format;
    }
    File longest = load(named(1, "\"" + std::string(63, 'n') + "\""));
    ASSERT_NE(longest, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(std::string(module_at(longest.get(), 0).name),
              std::string(63, 'n'));

    EXPECT_NE(refusal(named(3, "\"\""))
                  .find("ecu 'pcm''s name is 0 bytes; it takes 1 to 63"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_NE(refusal(named(3, "\"" + std::string(64, 'n') + "\""))
                  .find("ecu 'pcm''s name is 64 bytes; it takes 1 to 63"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_NE(refusal(named(3, "\"=1+1\""))
                  .find("ecu 'pcm''s name '=1+1' starts as a spreadsheet "
                        "formula would"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_NE(refusal(named(3, "\"a\\u0000b\""))
                  .find("ecu 'pcm''s name holds a NUL character"),
              std::string::npos)
        << ecuio_last_error_detail();
    EXPECT_NE(refusal(named(3, "7"))
                  .find("ecu 'pcm''s name is a number, not a string"),
              std::string::npos)
        << ecuio_last_error_detail();
}

TEST(SignalFileFormat3Test, AnEcusSessionIsReadInAnyFormat)
{
    const auto declared = [](unsigned format, const std::string& session) {
        return file_of(format, did("a", "A", "D100"),
                       R"("ecus":[{"id":"pcm","rx":"7E8"},{"id":"tcm",)"
                       R"("rx":"7E9","session":)" +
                           session + "}],");
    };
    for (unsigned format : {1u, 2u, 3u}) {
        File f = load(declared(format, "\"extended\""));
        ASSERT_NE(f, nullptr) << format << ": " << ecuio_last_error_detail();
        const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
        EXPECT_EQ(set->channels[0].session, ECUIO_CHANNEL_SESSION_NONE);
        EXPECT_EQ(set->channels[1].session, ECUIO_CHANNEL_SESSION_EXTENDED);
        EXPECT_EQ(module_at(f.get(), 0).session, ECUIO_CHANNEL_SESSION_NONE);
        EXPECT_EQ(module_at(f.get(), 1).session, ECUIO_CHANNEL_SESSION_EXTENDED)
            << format;
    }
    File d = load(declared(3, "\"default\""));
    ASSERT_NE(d, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(module_at(d.get(), 1).session, ECUIO_CHANNEL_SESSION_DEFAULT);

    /* An entry at the base's engine ECU declares the base channel's */
    File base = load(file_of(3, did("a", "A", "D100"),
                             R"("extends":"obd2","ecus":[{"id":"pcm",)"
                             R"("rx":"7E8","session":"extended"}],)"));
    ASSERT_NE(base, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_set(base.get())->channels[0].session,
              ECUIO_CHANNEL_SESSION_EXTENDED);

    const std::string programming =
        "ecu 'tcm''s session can't be the programming session: it is core's "
        "alone, entered only by a write operation, never by a signal file";
    const struct {
        const char* value;
        std::string words;
    } refused[] = {
        {"\"programming\"", programming},
        {"\"02\"", programming},
        {"\"Extended\"",
         "ecu 'tcm''s session is 'Extended': it takes \"default\" or "
         "\"extended\""},
        {"\"03\"", "ecu 'tcm''s session is '03': it takes \"default\" or "
                   "\"extended\""},
        {"\"\"", "ecu 'tcm''s session is 0 bytes; it takes 1 to 63"},
        {"3", "ecu 'tcm''s session is a number, not a string"},
    };
    for (const auto& r : refused) {
        EXPECT_NE(refusal(declared(3, r.value)).find(r.words),
                  std::string::npos)
            << r.value << ": " << ecuio_last_error_detail();
    }
}

// A name adds and changes nothing else: a format 2 file that names its
// ECU still replaces a base def by name
TEST(SignalFileFormat3Test, AnEcusNameLeavesAFormat2FilesReplacementAlone)
{
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    File f = load(file_of(
        2, did("map_kpa", "Intake Manifold Pressure", "D100"),
        R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8","name":"Engine"}],)"));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_signal_file_format(f.get()), 2u);
    EXPECT_EQ(ecuio_signal_file_set(f.get())->count, obd2->count)
        << "the file's signal stands in the base def's place";
    EXPECT_EQ(ecuio_signal_file_find(f.get(), "map_kpa")->id,
              ECUIO_OBD2_PID_SIG_ID(0x0B, 0));
    EXPECT_STREQ(module_at(f.get(), 0).name, "Engine");
}

TEST(SignalFileFormat3Test, AModuleIsItsEcusEntry)
{
    /* The base's engine ECU, unnamed by the file; then the file's two */
    File f = load(file_of(
        3,
        did("a", "A", "D100") + "," +
            R"({"key":"b","name":"B","ecu":"abs","read":{"service":"22",)"
            R"("number":"D100"},"field":{"byte":0}})",
        R"("extends":"obd2","ecus":[{"id":"tcm","rx":"7E9","name":"Transmission"},)"
        R"({"id":"abs","rx":"760","tx":"740"}],)"));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_signal_file_set(f.get())->channel_count, 3u);

    const ecuio_signal_module_t base = module_at(f.get(), 0);
    EXPECT_EQ(base.struct_size, sizeof base);
    EXPECT_EQ(base.flags, ECUIO_MODULE_BASE);
    EXPECT_EQ(base.rx_id, 0x7E8u);
    EXPECT_EQ(base.tx_id, 0x7E0u);
    EXPECT_EQ(base.part, ECUIO_SIGNAL_PART_NONE);
    EXPECT_STREQ(base.id, "");
    EXPECT_STREQ(base.name, "");

    const ecuio_signal_module_t tcm = module_at(f.get(), 1);
    EXPECT_EQ(tcm.flags, 0u);
    EXPECT_EQ(tcm.rx_id, 0x7E9u);
    EXPECT_EQ(tcm.tx_id, 0x7E1u);
    EXPECT_EQ(tcm.part, ECUIO_SIGNAL_PART_NONE);
    EXPECT_STREQ(tcm.id, "tcm");
    EXPECT_STREQ(tcm.name, "Transmission");
    for (uint32_t r : tcm.reserved) {
        EXPECT_EQ(r, 0u);
    }

    const ecuio_signal_module_t abs = module_at(f.get(), 2);
    EXPECT_EQ(abs.flags, 0u);
    EXPECT_EQ(abs.rx_id, 0x760u);
    EXPECT_EQ(abs.tx_id, 0x740u);
    EXPECT_STREQ(abs.id, "abs");
    EXPECT_STREQ(abs.name, "") << "an entry with no name";

    /* An entry at a base channel's IDs names that channel, wherever the
     * entry stands in the list */
    File g = load(file_of(1, did("a", "A", "D100"),
                          R"("extends":"obd2","ecus":[{"id":"tcm","rx":"7E9"},)"
                          R"({"id":"pcm","rx":"7E8","name":"Engine"}],)"));
    ASSERT_NE(g, nullptr) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_signal_file_set(g.get())->channel_count, 2u);
    const ecuio_signal_module_t pcm = module_at(g.get(), 0);
    EXPECT_EQ(pcm.flags, 0u) << "named by the file: not a bare base channel";
    EXPECT_EQ(pcm.rx_id, 0x7E8u);
    EXPECT_STREQ(pcm.id, "pcm");
    EXPECT_STREQ(pcm.name, "Engine");
    const ecuio_signal_module_t second = module_at(g.get(), 1);
    EXPECT_EQ(second.rx_id, 0x7E9u);
    EXPECT_STREQ(second.id, "tcm");
    EXPECT_STREQ(second.name, "");
}

TEST(SignalFileFormat3Test, TheModuleGetterFollowsItsContract)
{
    File f = load(file_of(3, did("a", "A", "D100")));
    ASSERT_NE(f, nullptr) << ecuio_last_error_detail();

    ecuio_signal_module_t m;
    std::memset(&m, 0x5A, sizeof m);
    const ecuio_signal_module_t untouched = m;
    EXPECT_EQ(ecuio_signal_file_module(nullptr, 0, &m),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_module(f.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the module needs a file and a struct to fill");

    /* A struct smaller than this core's is refused, untouched */
    m.struct_size = sizeof m - 1;
    ecuio_signal_module_t small = m;
    EXPECT_EQ(ecuio_signal_file_module(f.get(), 0, &m),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&m, &small, sizeof m), 0);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the module's struct_size is 135, under its 136 bytes");

    /* At the count and past it: refused, out untouched */
    m = untouched;
    m.struct_size = sizeof m;
    const ecuio_signal_module_t sized = m;
    EXPECT_EQ(ecuio_signal_file_module(f.get(), 1, &m),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the file's set has 1 modules, and index 1 is none");
    EXPECT_EQ(ecuio_signal_file_module(f.get(), UINT32_MAX, &m),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&m, &sized, sizeof m), 0);

    /* A larger struct is filled to this core's size, the rest kept */
    struct Wide {
        ecuio_signal_module_t m;
        uint32_t after;
    } wide;
    std::memset(&wide, 0x5A, sizeof wide);
    wide.m.struct_size = sizeof wide;
    EXPECT_EQ(ecuio_signal_file_module(f.get(), 0, &wide.m), ECUIO_OK);
    EXPECT_EQ(wide.m.struct_size, sizeof(ecuio_signal_module_t));
    EXPECT_EQ(wide.after, 0x5A5A5A5Au);
    EXPECT_STREQ(wide.m.id, "pcm");
    /* Every byte past each text is zero */
    for (size_t i = std::strlen(wide.m.id); i < sizeof wide.m.id; ++i) {
        EXPECT_EQ(wide.m.id[i], '\0');
    }
    for (char c : wide.m.name) {
        EXPECT_EQ(c, '\0');
    }
}
