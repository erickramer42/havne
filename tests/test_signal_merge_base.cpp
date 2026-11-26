// File/layer: tests/test_signal_merge_base.cpp | C++17, Qt-free, every platform
// Testing: UNIT: MergeTest (test_signals)
//
// A merge's base (ADR-0065 section 9, ecuio_signal_file_merge's options):
// obd2 whole, or its modules alone with ECUIO_MERGE_BASE_MODULES, so a
// vehicle that unpicks obd2 still reads the emissions codes.

#include "test_signal_merge_support.h"

namespace {

/* A body module at 7EA, read by DID */
const char* const kBody = R"([{"id":"body","rx":"7EA"}])";

/* Its text, loaded again at a set's bounds */
File read_back(const ecuio_signal_file_t* f)
{
    size_t length = 0;
    const char* text = ecuio_signal_file_text(f, &length);
    return load_set(std::string(text, length));
}

std::string text_of(const ecuio_signal_file_t* f)
{
    size_t length = 0;
    const char* text = ecuio_signal_file_text(f, &length);
    return std::string(text, length);
}

/* The set's channels as rx/tx pairs */
std::vector<std::pair<uint32_t, uint32_t>>
channels_of(const ecuio_signal_file_t* f)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f);
    for (uint32_t c = 0; c < set->channel_count; ++c) {
        out.emplace_back(set->channels[c].rx_id, set->channels[c].tx_id);
    }
    return out;
}

using Channels = std::vector<std::pair<uint32_t, uint32_t>>;

} // namespace

// obd2's channels come first and none of its signals: a part's signals
// alone, the base's module named by no part; with no part at all, and
// with every part left out, the set reads the modules and no signal
TEST(MergeTest, BaseModulesGivesObd2sChannelsAndNoSignal)
{
    File body = load(doc("Body", num("door", "D100"), "", 3, kBody));
    ASSERT_TRUE(body) << ecuio_last_error_detail();
    const Merged m = merge({body.get()}, ECUIO_MERGE_BASE_MODULES);
    ASSERT_EQ(m.status, ECUIO_OK) << m.words;
    const ecuio_signal_file_t* f = m.file.get();
    EXPECT_EQ(ecuio_signal_file_set(f)->count, 1u);
    EXPECT_EQ(keys_of(f), (Words{"door"}));
    EXPECT_STREQ(ecuio_signal_file_extends(f), "");
    EXPECT_EQ(channels_of(f), (Channels{{0x7E8, 0x7E0}, {0x7EA, 0x7E2}}));
    ecuio_signal_module_t base{};
    base.struct_size = sizeof base;
    ASSERT_EQ(ecuio_signal_file_module(f, 0, &base), ECUIO_OK);
    EXPECT_EQ(base.flags, ECUIO_MODULE_BASE);
    EXPECT_EQ(base.part, ECUIO_SIGNAL_PART_NONE);
    const std::string text = text_of(f);
    EXPECT_NE(text.find(R"("base_modules")"), std::string::npos) << text;
    EXPECT_EQ(text.find(R"("extends")"), std::string::npos) << text;

    /* No part: the modules alone, and a session can register the set */
    const Merged none = merge({}, ECUIO_MERGE_BASE_MODULES);
    ASSERT_EQ(none.status, ECUIO_OK) << none.words;
    const ecuio_signal_file_t* alone = none.file.get();
    EXPECT_EQ(ecuio_signal_file_set(alone)->count, 0u);
    EXPECT_EQ(ecuio_signal_file_part_count(alone), 0u);
    EXPECT_EQ(channels_of(alone), (Channels{{0x7E8, 0x7E0}}));
    EXPECT_STREQ(ecuio_signal_file_name(alone), "obd2's modules alone");
    const char* profile = ecuio_signal_file_profile(alone);
    EXPECT_EQ(
        ecuio_profile_register(profile, ecuio_signal_file_set(alone), nullptr),
        ECUIO_OK);
    EXPECT_EQ(ecuio_profile_unregister(profile), ECUIO_OK);
    /* Without the flag a merge needs a part */
    EXPECT_EQ(merge({}).words, "a merge takes at least one part");

    /* Every part left out (the other ID width): the modules still stand */
    File wide = load(doc("Wide", num("w", "D200"), "", 3,
                         R"([{"id":"bcm","rx":"18DAF140"}])"));
    ASSERT_TRUE(wide) << ecuio_last_error_detail();
    const Merged left = merge({wide.get()}, ECUIO_MERGE_BASE_MODULES);
    ASSERT_EQ(left.status, ECUIO_OK) << left.words;
    EXPECT_EQ(ecuio_signal_file_set(left.file.get())->count, 0u);
    EXPECT_EQ(merge({wide.get()}, ECUIO_MERGE_BASE_OBD2).words,
              "no signal of the 1 part is left to read");
}

// The two bases together are refused in words
TEST(MergeTest, BaseModulesWithBaseObd2IsRefused)
{
    File body = load(doc("Body", num("door", "D100"), "", 3, kBody));
    ASSERT_TRUE(body) << ecuio_last_error_detail();
    const Merged both =
        merge({body.get()}, ECUIO_MERGE_BASE_OBD2 | ECUIO_MERGE_BASE_MODULES);
    EXPECT_EQ(both.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(both.words,
              "a merge reads obd2 whole (ECUIO_MERGE_BASE_OBD2) or its modules "
              "alone (ECUIO_MERGE_BASE_MODULES), not both");
}

// A part that extends obd2 reads obd2's signals ("obd2.<key>"), so the
// set is obd2 whole, as BASE_OBD2 makes it
TEST(MergeTest, APartExtendingObd2MakesBaseModulesTheFullBase)
{
    File body = load(doc("Body", num("door", "D100"), "", 3, kBody));
    File fast = load(
        doc("Fast", calc("half_rpm", "obd2.rpm / 2"), R"("extends":"obd2",)"));
    ASSERT_TRUE(body && fast) << ecuio_last_error_detail();
    const Merged modules =
        merge({body.get(), fast.get()}, ECUIO_MERGE_BASE_MODULES);
    const Merged whole = merge({body.get(), fast.get()}, ECUIO_MERGE_BASE_OBD2);
    ASSERT_EQ(modules.status, ECUIO_OK) << modules.words;
    ASSERT_EQ(whole.status, ECUIO_OK) << whole.words;
    EXPECT_STREQ(ecuio_signal_file_extends(modules.file.get()), "obd2");
    EXPECT_EQ(ecuio_signal_file_set(modules.file.get())->count,
              ecuio_obd2_signal_set()->count + 2);
    EXPECT_NE(ecuio_signal_file_find(modules.file.get(), "half_rpm"), nullptr);
    EXPECT_EQ(text_of(modules.file.get()), text_of(whole.file.get()));
}

// Each base names its own profile, which a copy read back has too
TEST(MergeTest, BaseModulesNamesAnotherProfileThanTheFullBase)
{
    File body = load(doc("Body", num("door", "D100"), "", 3, kBody));
    ASSERT_TRUE(body) << ecuio_last_error_detail();
    const Merged modules = merge({body.get()}, ECUIO_MERGE_BASE_MODULES);
    const Merged whole = merge({body.get()}, ECUIO_MERGE_BASE_OBD2);
    const Merged plain = merge({body.get()});
    ASSERT_EQ(modules.status, ECUIO_OK) << modules.words;
    ASSERT_EQ(whole.status, ECUIO_OK) << whole.words;
    ASSERT_EQ(plain.status, ECUIO_OK) << plain.words;
    const std::string named = ecuio_signal_file_profile(modules.file.get());
    EXPECT_NE(named, ecuio_signal_file_profile(whole.file.get()));
    EXPECT_NE(named, ecuio_signal_file_profile(plain.file.get()));
    for (const Merged* m : {&modules, &whole, &plain}) {
        File back = read_back(m->file.get());
        ASSERT_TRUE(back);
        EXPECT_STREQ(ecuio_signal_file_profile(back.get()),
                     ecuio_signal_file_profile(m->file.get()));
        EXPECT_EQ(channels_of(back.get()), channels_of(m->file.get()));
        EXPECT_EQ(ecuio_signal_file_set(back.get())->count,
                  ecuio_signal_file_set(m->file.get())->count);
    }
    const Merged none = merge({}, ECUIO_MERGE_BASE_MODULES);
    ASSERT_EQ(none.status, ECUIO_OK) << none.words;
    File back = read_back(none.file.get());
    ASSERT_TRUE(back);
    EXPECT_STREQ(ecuio_signal_file_profile(back.get()),
                 ecuio_signal_file_profile(none.file.get()));
    EXPECT_EQ(channels_of(back.get()), (Channels{{0x7E8, 0x7E0}}));
}
