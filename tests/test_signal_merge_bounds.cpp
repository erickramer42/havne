// File/layer: tests/test_signal_merge_bounds.cpp | C++17, Qt-free, every platform
// Testing: UNIT: MergeTest (test_signals)
//
// ecuio_signal_file_merge's modules (ADR-0065 section 9: joined by their
// IDs, renamed, left out), the base it may be given, the set's bounds and
// the call's own refusals, each by its words.

#include "test_signal_merge_support.h"

#include "signal_file_build.h" /* core/src: a part with no text */

#include <cstdio>

namespace {

ecuio_signal_module_t module_at(const ecuio_signal_file_t* f, uint32_t index)
{
    ecuio_signal_module_t m{};
    m.struct_size = sizeof m;
    EXPECT_EQ(ecuio_signal_file_module(f, index, &m), ECUIO_OK)
        << ecuio_last_error_detail();
    return m;
}

/* n numbers s<first> and on, 16 to a DID from did, on the file's first ECU */
std::string numbers(unsigned first, unsigned n, unsigned did,
                    const char* prefix = "s")
{
    std::string out;
    for (unsigned i = 0; i < n; ++i) {
        char one[160];
        std::snprintf(
            one, sizeof one,
            "%s{\"key\":\"%s%u\",\"name\":\"S\",\"read\":{\"service\":"
            "\"22\",\"number\":\"%04X\"},\"field\":{\"byte\":%u}}",
            i == 0 ? "" : ",", prefix, first + i, did + i / 16, i % 16);
        out += one;
    }
    return out;
}

} // namespace

TEST(MergeTest, EcusAtTheSameIdsAreOneModule)
{
    File a = load(doc(
        "Alpha", num("a", "D100") + "," + num("t", "D101", R"(,"ecu":"tcm")"),
        "", 3, R"([{"id":"pcm","rx":"7E8"},{"id":"tcm","rx":"7E9"}])"));
    File b = load(
        doc("Beta",
            num("u", "D200") + "," + num("w", "D201", R"(,"ecu":"abs")"), "", 3,
            R"([{"id":"trans","rx":"7E9","name":"Transmission"},)"
            R"({"id":"abs","rx":"760","tx":"740","name":"Brakes"}])"));
    ASSERT_NE(a, nullptr) << ecuio_last_error_detail();
    ASSERT_NE(b, nullptr) << ecuio_last_error_detail();
    const Merged m = merge({a.get(), b.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_file_t* f = m.file.get();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f);
    ASSERT_EQ(set->channel_count, 3u);
    /* The earliest part's id stands, with the earliest name given */
    const ecuio_signal_module_t tcm = module_at(f, 1);
    EXPECT_STREQ(tcm.id, "tcm");
    EXPECT_STREQ(tcm.name, "Transmission");
    EXPECT_EQ(tcm.part, 0u);
    EXPECT_EQ(tcm.rx_id, 0x7E9u);
    const ecuio_signal_module_t abs = module_at(f, 2);
    EXPECT_STREQ(abs.id, "abs");
    EXPECT_EQ(abs.part, 1u);
    EXPECT_EQ(abs.tx_id, 0x740u);
    /* Beta's first ECU is the module Alpha calls tcm */
    EXPECT_EQ(ecuio_signal_file_find(f, "u")->message_id, 0x7E9u);
    EXPECT_EQ(ecuio_signal_file_find(f, "w")->message_id, 0x760u);
    EXPECT_EQ(ecuio_signal_file_note_count(f), 0u);
    ecuio_signal_part_t part{};
    part.struct_size = sizeof part;
    ASSERT_EQ(ecuio_signal_file_part(f, 1, &part), ECUIO_OK);
    EXPECT_EQ(part.module_count, 2u);

    /* A later part's name never replaces one given */
    File c = load(doc("Gamma", num("z", "D300"), "", 3,
                      R"([{"id":"transaxle","rx":"7E9","name":"Transaxle"}])"));
    const Merged named = merge({a.get(), b.get(), c.get()});
    ASSERT_EQ(named.status, ECUIO_OK) << named.words;
    EXPECT_STREQ(module_at(named.file.get(), 1).name, "Transmission");
}

TEST(MergeTest, AnIdAnotherModuleHoldsTakesASuffix)
{
    File a = load(doc("Alpha", num("a", "D100")));
    File b = load(
        doc("Beta", num("b", "D100"), "", 3, R"([{"id":"pcm","rx":"7E9"}])"));
    const Merged m = merge({a.get(), b.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_file_t* f = m.file.get();
    EXPECT_STREQ(module_at(f, 1).id, "pcm_2");
    EXPECT_EQ(keys_of(f), (Words{"a", "b"})) << "another module, so beside it";
    EXPECT_EQ(ecuio_signal_file_find(f, "b")->message_id, 0x7E9u);
    EXPECT_EQ(sentences(f),
              (Words{"5: ecu 'pcm' of 'Beta' is 'pcm_2' in the set, where "
                     "'pcm' is another module"}));
    EXPECT_EQ(notes_of(f)[0].part, 1u);
    EXPECT_EQ(notes_of(f)[0].def, UINT32_MAX);
}

TEST(MergeTest, AModuleTheSetCannotHoldIsLeftOutWithItsSignals)
{
    /* Its reply ID answers under another request ID */
    File a = load(doc("Alpha", num("a", "D100"), "", 3,
                      R"([{"id":"m","rx":"700","tx":"600"}])"));
    File b = load(doc("Beta",
                      num("b", "D100", R"(,"ecu":"m2")") + "," +
                          num("c", "D101", R"(,"ecu":"m2")") + "," +
                          num("d", "D102", R"(,"ecu":"ok")"),
                      "", 3,
                      R"([{"id":"ok","rx":"701","tx":"601"},)"
                      R"({"id":"m2","rx":"700","tx":"6FF"}])"));
    ASSERT_NE(b, nullptr) << ecuio_last_error_detail();
    const Merged m = merge({a.get(), b.get()});
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    EXPECT_EQ(keys_of(m.file.get()), (Words{"a", "d"}));
    EXPECT_EQ(sentences(m.file.get()),
              (Words{"6: ecu 'm2' of 'Beta' is left out with its 2 signals: "
                     "its reply ID 700 is answered under another request ID"}));
    EXPECT_EQ(notes_of(m.file.get())[0].count, 2u);
    EXPECT_EQ(notes_of(m.file.get())[0].part, 1u);

    /* A part of the other ID width, whole */
    File wide = load(doc("Wide",
                         num("w", "D100") + "," + num("v", "D101") + "," +
                             calc("k", "w + v"),
                         "", 3, R"([{"id":"x","rx":"18DAF110"}])"));
    ASSERT_NE(wide, nullptr) << ecuio_last_error_detail();
    const Merged width = merge({a.get(), wide.get()});
    ASSERT_EQ(width.status, ECUIO_OK) << width.words;
    EXPECT_EQ(keys_of(width.file.get()), (Words{"a"}));
    EXPECT_EQ(sentences(width.file.get()),
              (Words{"7: 'Wide' is left out with its 3 signals: its IDs are "
                     "29-bit, and the set's are 11-bit"}));
    EXPECT_EQ(notes_of(width.file.get())[0].count, 3u);
    /* It stays a part: the name follows what was asked for */
    EXPECT_EQ(ecuio_signal_file_part_count(width.file.get()), 2u);
    /* The first part sets the width, not the last */
    const Merged other = merge({wide.get(), a.get()});
    ASSERT_EQ(other.status, ECUIO_OK) << other.words;
    EXPECT_EQ(sentences(other.file.get()),
              (Words{"7: 'Alpha' is left out with its 1 signal: its IDs are "
                     "11-bit, and the set's are 29-bit"}));

    /* Past ECUIO_MAX_CHANNELS modules */
    std::string ecus_a = "[";
    std::string ecus_b = "[";
    std::string signals_a;
    std::string signals_b;
    for (unsigned i = 0; i < 20; ++i) {
        char one[200];
        std::snprintf(one, sizeof one,
                      "%s{\"id\":\"a%u\",\"rx\":\"%X\",\"tx\":\"%X\"}",
                      i == 0 ? "" : ",", i, 0x620u + i, 0x600u + i);
        ecus_a += one;
        std::snprintf(one, sizeof one,
                      "%s{\"id\":\"b%u\",\"rx\":\"%X\",\"tx\":\"%X\"}",
                      i == 0 ? "" : ",", i, 0x660u + i, 0x640u + i);
        ecus_b += one;
        signals_a += (i == 0 ? "" : ",") +
                     num("sa" + std::to_string(i), "D100",
                         ",\"ecu\":\"a" + std::to_string(i) + "\"");
        signals_b += (i == 0 ? "" : ",") +
                     num("sb" + std::to_string(i), "D100",
                         ",\"ecu\":\"b" + std::to_string(i) + "\"");
    }
    File many_a = load(doc("Many", signals_a, "", 3, ecus_a + "]"));
    File many_b = load(doc("More", signals_b, "", 3, ecus_b + "]"));
    ASSERT_NE(many_a, nullptr) << ecuio_last_error_detail();
    ASSERT_NE(many_b, nullptr) << ecuio_last_error_detail();
    const Merged full = merge({many_a.get(), many_b.get()});
    ASSERT_EQ(full.status, ECUIO_OK) << full.words;
    EXPECT_EQ(ecuio_signal_file_set(full.file.get())->channel_count,
              uint32_t{ECUIO_MAX_CHANNELS});
    EXPECT_EQ(keys_of(full.file.get()).size(), 32u);
    const Words said = sentences(full.file.get());
    ASSERT_EQ(said.size(), 8u);
    EXPECT_EQ(said[0], "6: ecu 'b12' of 'More' is left out with its 1 signal: "
                       "the set holds 32 modules, the base's counted");
}

TEST(MergeTest, TheBaseIsObd2WhenAPartExtendsItOrTheCallerAsks)
{
    File a = load(doc("Alpha", num("a", "D100")));
    const Merged plain = merge({a.get()});
    ASSERT_EQ(plain.status, ECUIO_OK) << plain.words;
    EXPECT_EQ(ecuio_signal_file_set(plain.file.get())->count, 1u);
    /* sha256("user.alpha\n") */
    EXPECT_STREQ(ecuio_signal_file_profile(plain.file.get()),
                 "merged.6fd7f1660a97c984");

    const Merged based = merge({a.get()}, ECUIO_MERGE_BASE_OBD2);
    ASSERT_EQ(based.status, ECUIO_OK) << based.words;
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(based.file.get());
    ASSERT_EQ(set->count, obd2->count + 1);
    EXPECT_EQ(set->defs[0].id, obd2->defs[0].id);
    /* sha256("obd2\nuser.alpha\n") */
    EXPECT_STREQ(ecuio_signal_file_profile(based.file.get()),
                 "merged.86c7d9913f5ab952");
    /* The part's ECU names obd2's first channel */
    EXPECT_STREQ(module_at(based.file.get(), 0).id, "pcm");
    EXPECT_EQ(module_at(based.file.get(), 0).flags, 0u);

    /* A part that reads the base's ECU by naming none gives it an id */
    File bare =
        load(doc("Bare", num("b", "D100"), R"("extends":"obd2",)", 3, ""));
    ASSERT_NE(bare, nullptr) << ecuio_last_error_detail();
    const Merged unnamed = merge({bare.get()});
    ASSERT_EQ(unnamed.status, ECUIO_OK) << unnamed.words;
    EXPECT_STREQ(module_at(unnamed.file.get(), 0).id, "obd2");
    EXPECT_EQ(module_at(unnamed.file.get(), 0).part, ECUIO_SIGNAL_PART_NONE);
    EXPECT_EQ(ecuio_signal_file_find(unnamed.file.get(), "b")->message_id,
              0x7E8u);
    /* A part that names the ECU gives the id, whichever comes first */
    const Merged later = merge({bare.get(), a.get()});
    ASSERT_EQ(later.status, ECUIO_OK) << later.words;
    EXPECT_STREQ(module_at(later.file.get(), 0).id, "pcm");
    EXPECT_EQ(module_at(later.file.get(), 0).part, 1u);
    EXPECT_EQ(keys_of(later.file.get()), (Words{"a"}))
        << "one address, so Alpha's a took b's place";
}

TEST(MergeTest, TheCallRefusesWhatItIsGivenInWords)
{
    File a = load(doc("Alpha", num("a", "D100")));
    File b = load(doc("Beta", num("b", "D101")));
    const ecuio_signal_file_t* two[] = {a.get(), b.get()};
    ecuio_signal_file_t* out = reinterpret_cast<ecuio_signal_file_t*>(1);

    EXPECT_EQ(ecuio_signal_file_merge(two, 2, nullptr, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_signal_file_merge(nullptr, 2, nullptr, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    const ecuio_signal_file_t* holed[] = {a.get(), nullptr};
    out = reinterpret_cast<ecuio_signal_file_t*>(1);
    EXPECT_EQ(ecuio_signal_file_merge(holed, 2, nullptr, &out),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(), "part 1 is NULL");

    out = reinterpret_cast<ecuio_signal_file_t*>(1);
    EXPECT_EQ(ecuio_signal_file_merge(nullptr, 0, nullptr, &out),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(), "a merge takes at least one part");

    std::vector<File> files;
    std::vector<const ecuio_signal_file_t*> many;
    for (int i = 0; i < 17; ++i) {
        files.push_back(load(doc("P" + std::to_string(i),
                                 num("k" + std::to_string(i), "D100"))));
        many.push_back(files.back().get());
    }
    out = reinterpret_cast<ecuio_signal_file_t*>(1);
    EXPECT_EQ(ecuio_signal_file_merge(many.data(), 17, nullptr, &out),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "17 parts are more than the 16 a merged set holds");
    EXPECT_EQ(ecuio_signal_file_merge(many.data(), 16, nullptr, &out), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(keys_of(out), (Words{"k15"})) << "one address: the last stands";
    ecuio_signal_file_free(out);

    EXPECT_EQ(merge({a.get(), a.get()}).words,
              "part 1 is part 0 again: a set holds a part once");
    File same = load(doc("Alpha", num("z", "D300")));
    EXPECT_EQ(merge({a.get(), same.get()}).words,
              "part 1 has the profile of part 0, 'user.alpha': a set holds a "
              "part once");
    const Merged made = merge({a.get(), b.get()});
    ASSERT_EQ(made.status, ECUIO_OK);
    const Merged nested = merge({made.file.get(), same.get()});
    EXPECT_EQ(nested.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(nested.words, "part 0 is a merged set: merge its parts instead");
    File textless = load(doc("Gamma", num("g", "D400")));
    textless->bytes.clear();
    EXPECT_EQ(merge({a.get(), textless.get()}).words,
              "part 1 has no text to merge");

    /* The options */
    ecuio_merge_options_t options{};
    options.struct_size = sizeof options - 1;
    out = reinterpret_cast<ecuio_signal_file_t*>(1);
    EXPECT_EQ(ecuio_signal_file_merge(two, 2, &options, &out),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the options' struct_size is 31, under its 32 bytes");
    options.struct_size = sizeof options;
    options.flags = 0xC;
    EXPECT_EQ(ecuio_signal_file_merge(two, 2, &options, &out),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(), "a merge takes no flag 0xc");
    options.flags = 0;
    options.reserved[5] = 1;
    EXPECT_EQ(ecuio_signal_file_merge(two, 2, &options, &out),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the options' reserved words are zero");
    options.reserved[5] = 0;
    options.struct_size = sizeof options + 8;
    EXPECT_EQ(ecuio_signal_file_merge(two, 2, &options, &out), ECUIO_OK)
        << "a later caller's larger struct";
    ecuio_signal_file_free(out);
}

TEST(MergeTest, ABoundOfTheSetPassedIsRefusedNamingItsCount)
{
    /* 16,400 signals of two modules */
    const std::string pcm = R"([{"id":"pcm","rx":"7E8"}])";
    const std::string tcm = R"([{"id":"tcm","rx":"7E9"}])";
    File half = load_set(doc("Half", numbers(0, 8200, 0xA000), "", 3, pcm));
    File rest = load_set(doc("Rest", numbers(8200, 8200, 0xA000), "", 3, tcm));
    ASSERT_NE(half, nullptr);
    ASSERT_NE(rest, nullptr);
    const Merged signals = merge({half.get(), rest.get()});
    EXPECT_EQ(signals.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(signals.words,
              "the parts hold 16400 signals together, more than 16384");
    /* 16,384 are taken */
    File fits = load_set(doc("Rest", numbers(8200, 8184, 0xA000), "", 3, tcm));
    const Merged at_bound = merge({half.get(), fits.get()});
    ASSERT_EQ(at_bound.status, ECUIO_OK) << at_bound.words;
    EXPECT_EQ(ecuio_signal_file_set(at_bound.file.get())->count, 16384u);

    /* One module asked 1,200 requests by two parts that each fit */
    std::string dids_a;
    std::string dids_b;
    for (unsigned i = 0; i < 600; ++i) {
        char did[8];
        std::snprintf(did, sizeof did, "%04X", 0xB000u + i);
        dids_a += (i == 0 ? "" : ",") + num("a" + std::to_string(i), did);
        std::snprintf(did, sizeof did, "%04X", 0xC000u + i);
        dids_b += (i == 0 ? "" : ",") + num("b" + std::to_string(i), did);
    }
    File asks_a = load(doc("Asks", dids_a));
    File asks_b = load(doc("More", dids_b));
    ASSERT_NE(asks_a, nullptr) << ecuio_last_error_detail();
    const Merged requests = merge({asks_a.get(), asks_b.get()});
    EXPECT_EQ(requests.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(requests.words, "the parts ask PCM 1200 requests, more than "
                              "1024, the base's counted");
    /* The base's own requests count: obd2's with 1,000 more pass 1,024 */
    std::string thousand;
    for (unsigned i = 0; i < 1000; ++i) {
        char did[8];
        std::snprintf(did, sizeof did, "%04X", 0xB000u + i);
        thousand += (i == 0 ? "" : ",") + num("t" + std::to_string(i), did);
    }
    File near = load(doc("Near", thousand));
    ASSERT_NE(near, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(merge({near.get()}).status, ECUIO_OK);
    const Merged with_base = merge({near.get()}, ECUIO_MERGE_BASE_OBD2);
    EXPECT_EQ(with_base.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(has(with_base.words, "the parts ask PCM ")) << with_base.words;
    EXPECT_TRUE(
        has(with_base.words, " requests, more than 1024, the base's counted"))
        << with_base.words;

    /* A text past 4 MiB: each part's notes fit, the two don't */
    const std::string heavy(2200000, 'n');
    File big_a =
        load_set(doc("Big", num("a", "D100", ",\"notes\":\"" + heavy + "\"")));
    File big_b = load_set(
        doc("Bigger", num("b", "D101", ",\"notes\":\"" + heavy + "\"")));
    ASSERT_NE(big_a, nullptr);
    const Merged bytes = merge({big_a.get(), big_b.get()});
    EXPECT_EQ(bytes.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(has(bytes.words, "the merged set's text is 44")) << bytes.words;
    EXPECT_TRUE(has(bytes.words, " bytes, more than 4194304")) << bytes.words;
}

TEST(MergeTest, TheTablesBoundsAreRefusedNamingTheirCounts)
{
    /* 65,792 enum labels: 257 enums of 256. The last of a run holds
     * `fewer` fewer */
    const auto enums = [](unsigned first, unsigned n, unsigned fewer = 0) {
        const auto values = [](unsigned count) {
            std::string out;
            for (unsigned v = 0; v < count; ++v) {
                out += (v == 0 ? "\"" : ",\"") + std::to_string(v) + "\":\"L\"";
            }
            return out;
        };
        const std::string whole = values(256);
        std::string out;
        for (unsigned i = 0; i < n; ++i) {
            char head[160];
            std::snprintf(head, sizeof head,
                          "%s{\"key\":\"e%u\",\"name\":\"E\",\"kind\":\"enum\","
                          "\"read\":{\"service\":\"22\",\"number\":\"%04X\"},"
                          "\"field\":{\"byte\":%u},\"values\":{",
                          i == 0 ? "" : ",", first + i,
                          0xA000u + (first + i) / 16, (first + i) % 16);
            out += head + (i + 1 == n ? values(256 - fewer) : whole) + "}}";
        }
        return out;
    };
    File labels_a = load(doc("Labels", enums(0, 129)));
    File labels_b = load(doc("Others", enums(129, 128)));
    ASSERT_NE(labels_a, nullptr) << ecuio_last_error_detail();
    ASSERT_NE(labels_b, nullptr) << ecuio_last_error_detail();
    const Merged labels = merge({labels_a.get(), labels_b.get()});
    EXPECT_EQ(labels.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(labels.words, "the parts' enums hold 65792 labels together, "
                            "more than 65536");
    /* 65,536 with the base's own are taken, and one more isn't */
    const unsigned of_base = ecuio_obd2_signal_set()->enum_count;
    ASSERT_LT(of_base, 256u);
    File fill_a = load(doc("Fill", enums(0, 128)));
    File fill_b = load(doc("Filled", enums(128, 128, of_base)));
    File one = load(doc("One", enums(256, 1, 255)));
    ASSERT_NE(fill_b, nullptr) << ecuio_last_error_detail();
    ASSERT_NE(one, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(merge({fill_a.get(), fill_b.get()}, ECUIO_MERGE_BASE_OBD2).status,
              ECUIO_OK);
    const Merged past =
        merge({fill_a.get(), fill_b.get(), one.get()}, ECUIO_MERGE_BASE_OBD2);
    EXPECT_EQ(past.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(past.words, "the parts' enums hold 65537 labels together, "
                          "more than 65536");

    /* 18,000 replaced keys: four parts over the same 6,000 addresses */
    File layer[4];
    const char* const prefix[4] = {"a", "b", "c", "d"};
    for (int p = 0; p < 4; ++p) {
        layer[p] = load_set(doc(std::string("Layer ") + prefix[p],
                                numbers(0, 6000, 0xA000, prefix[p])));
        ASSERT_NE(layer[p], nullptr);
    }
    const Merged three =
        merge({layer[0].get(), layer[1].get(), layer[2].get()});
    ASSERT_EQ(three.status, ECUIO_OK) << three.words;
    EXPECT_EQ(replaced_keys(three.file.get(), "c0"), (Words{"b0", "a0"}));
    const Merged four =
        merge({layer[0].get(), layer[1].get(), layer[2].get(), layer[3].get()});
    EXPECT_EQ(four.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(four.words,
              "the parts' signals replaced 18000 keys, more than 16384");
}

namespace {

ecuio_signal_note_t note_at(const ecuio_signal_file_t* f, uint32_t index)
{
    ecuio_signal_note_t n{};
    n.struct_size = sizeof n;
    EXPECT_EQ(ecuio_signal_file_note(f, index, &n), ECUIO_OK);
    return n;
}

const char* const kTcmExtended =
    R"([{"id":"tcm","rx":"7E9","name":"Transmission","session":"extended"}])";
const char* const kTcmDefault =
    R"([{"id":"tcm","rx":"7E9","session":"default"}])";
const char* const kTcm = R"([{"id":"tcm","rx":"7E9"}])";

} // namespace

TEST(MergeTest, AnExplicitDefaultFromAnyPartWins)
{
    File asks = load(doc("Asks", num("a", "D100") + "," + num("b", "D101"), "",
                         3, kTcmExtended));
    File keeps = load(doc("Keeps", num("c", "D200"), "", 3, kTcmDefault));
    ASSERT_NE(asks, nullptr) << ecuio_last_error_detail();
    ASSERT_NE(keeps, nullptr) << ecuio_last_error_detail();
    for (const bool asks_first : {true, false}) {
        SCOPED_TRACE(asks_first ? "asks first" : "keeps first");
        const Merged m = asks_first ? merge({asks.get(), keeps.get()})
                                    : merge({keeps.get(), asks.get()});
        ASSERT_EQ(m.status, ECUIO_OK) << m.words;
        const ecuio_signal_file_t* f = m.file.get();
        EXPECT_EQ(ecuio_signal_file_set(f)->channels[0].session,
                  ECUIO_CHANNEL_SESSION_DEFAULT);
        EXPECT_EQ(module_at(f, 0).session, ECUIO_CHANNEL_SESSION_DEFAULT);
        ASSERT_EQ(ecuio_signal_file_note_count(f), 1u);
        const ecuio_signal_note_t n = note_at(f, 0);
        EXPECT_EQ(n.kind, ECUIO_NOTE_SESSION_OVERRULED);
        EXPECT_EQ(n.part, asks_first ? 0u : 1u);
        EXPECT_EQ(n.other_part, asks_first ? 1u : 0u);
        EXPECT_EQ(n.count, 2u);
        EXPECT_EQ(n.def, UINT32_MAX);
        EXPECT_STREQ(n.text, "Transmission (7E9) is read in the default "
                             "session ('Keeps' says so); 2 signals of 'Asks' "
                             "on it ask the extended session");
    }
}

TEST(MergeTest, APartWithNoSessionOverrulesNothing)
{
    File asks = load(doc("Asks", num("a", "D100"), "", 3, kTcmExtended));
    File silent = load(doc("Silent", num("c", "D200"), "", 3, kTcm));
    ASSERT_NE(asks, nullptr) << ecuio_last_error_detail();
    ASSERT_NE(silent, nullptr) << ecuio_last_error_detail();
    for (const Merged& m : {merge({asks.get(), silent.get()}),
                            merge({silent.get(), asks.get()})}) {
        ASSERT_EQ(m.status, ECUIO_OK) << m.words;
        EXPECT_EQ(module_at(m.file.get(), 0).session,
                  ECUIO_CHANNEL_SESSION_EXTENDED);
        EXPECT_EQ(ecuio_signal_file_note_count(m.file.get()), 0u);
    }
    const Merged none = merge({silent.get()});
    ASSERT_EQ(none.status, ECUIO_OK) << none.words;
    EXPECT_EQ(module_at(none.file.get(), 0).session,
              ECUIO_CHANNEL_SESSION_NONE);
}

TEST(MergeTest, AModuleTheStandardSignalsReadHasANoteForItsSession)
{
    File engine =
        load(doc("Engine", num("a", "D100"), "", 3,
                 R"([{"id":"pcm","rx":"7E8","session":"extended"}])"));
    ASSERT_NE(engine, nullptr) << ecuio_last_error_detail();
    const Merged with_base = merge({engine.get()}, ECUIO_MERGE_BASE_OBD2);
    ASSERT_EQ(with_base.status, ECUIO_OK) << with_base.words;
    const ecuio_signal_file_t* f = with_base.file.get();
    EXPECT_EQ(module_at(f, 0).session, ECUIO_CHANNEL_SESSION_EXTENDED)
        << "the declaration stands: the attach judges it";
    ASSERT_EQ(ecuio_signal_file_note_count(f), 1u);
    const ecuio_signal_note_t n = note_at(f, 0);
    EXPECT_EQ(n.kind, ECUIO_NOTE_SESSION_STANDARD);
    EXPECT_EQ(n.part, 0u);
    EXPECT_EQ(n.other_part, ECUIO_SIGNAL_PART_NONE);
    EXPECT_EQ(n.count, 1u);
    EXPECT_STREQ(n.text, "PCM (7E8) is read in the default session: the "
                         "standard signals are read from it; 1 signal of "
                         "'Engine' on it asks the extended session");

    const Merged alone = merge({engine.get()});
    ASSERT_EQ(alone.status, ECUIO_OK) << alone.words;
    EXPECT_EQ(ecuio_signal_file_note_count(alone.file.get()), 0u)
        << "no standard signal is read without obd2";

    File pid = load(
        doc("Pid",
            R"({"key":"p","name":"P","read":{"service":"01","number":"0C"},)"
            R"("field":{"byte":0}})",
            "", 3, R"([{"id":"tcm","rx":"7E9","session":"extended"}])"));
    ASSERT_NE(pid, nullptr) << ecuio_last_error_detail();
    const Merged own = merge({pid.get()});
    ASSERT_EQ(own.status, ECUIO_OK) << own.words;
    ASSERT_EQ(ecuio_signal_file_note_count(own.file.get()), 1u)
        << "a part's own Service 01 signal is a standard one";
    EXPECT_EQ(note_at(own.file.get(), 0).kind, ECUIO_NOTE_SESSION_STANDARD);
}
