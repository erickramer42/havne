#include "default_pick.h"
#include "no_room.h"

#include "ecuio/core.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

struct File {
    ecuio_signal_file_t* f = nullptr;
    explicit File(const std::string& text)
    {
        EXPECT_EQ(ecuio_signal_file_load(text.data(), text.size(), &f),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~File() { ecuio_signal_file_free(f); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

const char kRoom[] =
    R"({"havne_signals":1,"name":"Room","source":{"name":"t"},"ecus":[)"
    R"({"id":"m1","rx":"7E9","name":"M1"},{"id":"door","rx":"7EC"}],)"
    R"("signals":[)"
    R"({"key":"s1","name":"N s1","ecu":"m1","read":{"service":"22","number":"1000"},"field":{"byte":0}},)"
    R"({"key":"s4","name":"N s4","ecu":"door","read":{"service":"22","number":"1003"},"field":{"byte":0}},)"
    R"({"key":"s5","name":"N s5","ecu":"door","read":{"service":"22","number":"1004"},"field":{"byte":0}}]})";

} // namespace

TEST(CliPickTest, TheBesideBaseIdsAreObd2sDefsTheNotesName)
{
    File part(R"({"havne_signals":2,"name":"Engine","source":{"name":"t"},)"
              R"("extends":"obd2","ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)"
              R"({"key":"rpm","name":"RPM","ecu":"pcm","read":{"service":"22",)"
              R"("number":"F40C"},"field":{"byte":0,"bits":16},"plot":1}]})");
    const ecuio_signal_file_t* parts[] = {part.f};
    ecuio_signal_file_t* merged = nullptr;
    ASSERT_EQ(ecuio_signal_file_merge(parts, 1, nullptr, &merged), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(cli::beside_base(merged),
              std::vector<uint16_t>{ECUIO_OBD2_SIG_RPM_ID});
    const std::vector<uint16_t> pick = cli::default_pick(
        ecuio_signal_file_set(merged), {}, false, cli::beside_base(merged));
    const ecuio_signal_def_t* own = ecuio_signal_file_find(merged, "rpm");
    ASSERT_NE(own, nullptr);
    EXPECT_NE(std::find(pick.begin(), pick.end(), own->id), pick.end());
    EXPECT_EQ(std::find(pick.begin(), pick.end(), ECUIO_OBD2_SIG_RPM_ID),
              pick.end());
    ecuio_signal_file_free(merged);
    EXPECT_TRUE(cli::beside_base(part.f).empty()) << "a file merged from none";
}

TEST(CliLogTest, TheNoRoomLineNamesEachSignalByItsModule)
{
    File room(kRoom);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(room.f);
    ASSERT_EQ(set->channel_count, 2u);
    ecuio_channel_support_t reports[2] = {};
    EXPECT_TRUE(cli::no_room_names(set, room.f, {}, reports, 2).empty());
    reports[1].flags = ECUIO_SUPPORT_NO_ROOM;
    const std::vector<uint16_t> ids{set->defs[0].id, set->defs[1].id};
    EXPECT_EQ(cli::no_room_names(set, room.f, ids, reports, 2),
              std::vector<std::string>{"DOOR's N s4"});
    EXPECT_EQ(cli::no_room_names(set, room.f, {}, reports, 2),
              (std::vector<std::string>{"DOOR's N s4", "DOOR's N s5"}))
        << "no focus: the whole set is asked";
    EXPECT_EQ(cli::no_room_names(set, room.f, {set->defs[0].id}, reports, 2),
              std::vector<std::string>{});
    EXPECT_EQ(cli::no_room_names(set, nullptr, ids, reports, 2),
              std::vector<std::string>{"7EC's N s4"})
        << "no file: the reply ID";
    EXPECT_EQ(cli::module_name(set, room.f, 0), "M1");
    EXPECT_EQ(cli::no_room_text({"DOOR's N s4"}),
              "logging 1 signal the adapter has no room for: DOOR's N s4: "
              "their columns stay empty");
    EXPECT_EQ(cli::no_room_text({"A's x", "B's y"}),
              "logging 2 signals the adapter has no room for: A's x, B's y: "
              "their columns stay empty");
    EXPECT_EQ(cli::no_room_text({}), "");
}
