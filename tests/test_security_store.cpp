// File/layer: tests/test_security_store.cpp | test_core, Qt-free
// Testing: UNIT: SecurityStoreTest (security_store.{h,cpp}: the entries in
//          memory, the reader's rules and words, a vehicle's number; the
//          file's cases are in test_security_store_file.cpp)

#include "security_store.h"

#include "security_store_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>

using namespace security_store_test;

TEST(SecurityStoreTest, AnEntryIsFoundByItsVehicleAddressAndLevel)
{
    store s;
    EXPECT_EQ(store_find(s, kCar, kRx, 0x01), nullptr);
    store_entry* a = store_entry_for(s, kCar, kRx, 0x01);
    ASSERT_NE(a, nullptr);
    a->failed = 4;
    EXPECT_EQ(store_entry_for(s, kCar, kRx, 0x01),
              store_find(s, kCar, kRx, 0x01));
    EXPECT_EQ(s.entries.size(), 1u) << "made once";
    EXPECT_EQ(store_find(s, kCar, kRx, 0x03), nullptr) << "another level";
    EXPECT_EQ(store_find(s, kCar, kRx + 1, 0x01), nullptr) << "another module";
    EXPECT_EQ(store_find(s, kOtherCar, kRx, 0x01), nullptr)
        << "another vehicle with that address and level";
    store_entry* b = store_entry_for(s, kCar, kRx, 0x03);
    ASSERT_NE(b, nullptr);
    b->failed = 1;
    EXPECT_EQ(s.entries.size(), 2u);
    store_entry* other = store_entry_for(s, kOtherCar, kRx, 0x01);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(other->vehicle, kOtherCar);
    EXPECT_EQ(other->failed, 0u) << "a car's count is its own";
    EXPECT_EQ(s.entries.size(), 3u);
    EXPECT_EQ(store_find(s, kCar, kRx, 0x01)->failed, 4u);
}

/* The mark before the seed, and each end of the attempt */
TEST(SecurityStoreTest, AnAttemptsMarkAndItsEnds)
{
    store_entry e = entry(kRx, 0x01, 2, kUtc);
    store_attempt_began(e, kUtc + 100);
    EXPECT_EQ(e.pending_utc, kUtc + 100);
    EXPECT_EQ(e.failed, 2u) << "nothing counted yet";
    EXPECT_EQ(e.last_utc, kUtc);
    /* Nothing counted, no delay: as before the attempt */
    store_entry quiet = e;
    store_attempt_ended(quiet, false, false, kUtc + 101);
    EXPECT_EQ(quiet.pending_utc, 0);
    EXPECT_EQ(quiet.failed, 2u);
    EXPECT_EQ(quiet.last_utc, kUtc);
    /* A seed's 36 or 37: the delay runs, nothing counted */
    store_entry delayed = e;
    store_attempt_ended(delayed, false, true, kUtc + 101);
    EXPECT_EQ(delayed.pending_utc, 0);
    EXPECT_EQ(delayed.failed, 2u);
    EXPECT_EQ(delayed.last_utc, kUtc + 101);
    /* A failed key: one more, dated */
    store_entry failed = e;
    store_attempt_ended(failed, true, false, kUtc + 101);
    EXPECT_EQ(failed.pending_utc, 0);
    EXPECT_EQ(failed.failed, 3u);
    EXPECT_EQ(failed.last_utc, kUtc + 101) << "a failure's delay always runs";
    /* The right key */
    store_unlocked(e);
    EXPECT_EQ(e.failed, 0u);
    EXPECT_EQ(e.last_utc, 0);
    EXPECT_EQ(e.pending_utc, 0);
    /* The count stops at the file's bound; a clock at or before 1970 still
     * marks and dates */
    store_entry top = entry(kRx, 0x01, kStoreMaxFailed, kUtc);
    store_attempt_ended(top, true, false, 0);
    EXPECT_EQ(top.failed, kStoreMaxFailed);
    EXPECT_EQ(top.last_utc, 1);
    store_attempt_began(top, -5);
    EXPECT_EQ(top.pending_utc, 1);
    store_attempt_began(top, kStoreMaxUtc + 1);
    EXPECT_EQ(top.pending_utc, kStoreMaxUtc) << "what the file takes back";
    store_attempt_ended(top, false, true, kStoreMaxUtc + 1);
    EXPECT_EQ(top.last_utc, kStoreMaxUtc);
}

/* A file that doesn't read is refused in its words at its place, never
 * taken as empty: here the store's own shape */
TEST(SecurityStoreTest, AStoreOfAnotherShapeIsRefusedInItsWords)
{
    const std::string e = kEntry;
    EXPECT_FALSE(refusal("").empty()) << "an empty file isn't an empty store";
    expect_refused({
        {"[]", "line 1, column 1: a security store is a list, not an object"},
        {"{\"entries\": []}", "line 1, column 1: a security store needs "
                              "\"havne_security_store\": 1"},
        {"{\"havne_security_store\": 2, \"entries\": []}",
         "line 1, column 26: this Havne reads a security store of format 1 "
         "only"},
        {"{\"havne_security_store\": 1}",
         "line 1, column 1: a security store needs \"entries\""},
        {"{\"havne_security_store\": 1, \"entries\": {}}",
         "line 1, column 40: a security store's entries is an object, not a "
         "list"},
        {"{\"havne_security_store\": 1, \"entries\": [], \"more\": 1}",
         "line 1, column 44: unknown key 'more' in a security store"},
        /* A later format is refused as that, whatever keys it brings */
        {"{\"havne_security_store\": 2, \"entries\": [], \"more\": 1}",
         "line 1, column 26: this Havne reads a security store of format 1 "
         "only"},
        {with_entries("7"), "line 2, column 14: an entry is a number, not an "
                            "object"},
        {with_entries(e + ", " + e),
         "line 2, column 127: an entry's vehicle, rx and level are given "
         "twice"},
    });
    EXPECT_EQ(refusal(with_entries(e)), "")
        << "the entry the rows are cut from";
    EXPECT_EQ(refusal(with_entries("")), "") << "no entries";
    std::string other = kEntry;
    other.replace(other.find("0123456789ABCDEF"), 16, "FEDCBA9876543210");
    EXPECT_EQ(refusal(with_entries(e + ", " + other)), "")
        << "two vehicles with one address and level";
}

/* An entry holds its six keys and no other */
TEST(SecurityStoreTest, AnEntryMissingAKeyOrWithAnotherIsRefused)
{
    expect_refused({
        {entry_with("\"rx\": \"7E8\", ", ""),
         "line 2, column 14: an entry needs \"rx\""},
        {entry_with("\"level\": 1, ", ""),
         "line 2, column 14: an entry needs \"level\""},
        {entry_with("\"failed\": 2, ", ""),
         "line 2, column 14: an entry needs \"failed\""},
        {entry_with("\"last_utc\": 1795132800, ", ""),
         "line 2, column 14: an entry needs \"last_utc\""},
        {entry_with(", \"pending_utc\": 0", ""),
         "line 2, column 14: an entry needs \"pending_utc\""},
        {entry_with(", \"vehicle\": \"0123456789ABCDEF\"", ""),
         "line 2, column 14: an entry needs \"vehicle\""},
        {entry_with("\"pending_utc\": 0", "\"pending\": false"),
         "line 2, column 77: unknown key 'pending' in an entry"},
    });
}

/* An rx has one spelling: upper-case hex with no leading zero, 1 to
 * 1FFFFFFF */
TEST(SecurityStoreTest, AnEntrysRxIsOneSpellingOfAnId)
{
    expect_refused({
        {entry_with("\"7E8\"", "2024"),
         "line 2, column 21: an entry's rx is a number, not a string"},
        {entry_with("\"7E8\"", "\"\""),
         "line 2, column 21: an entry's rx is 0 bytes; it takes 1 to 8"},
        {entry_with("\"7E8\"", "\"123456789\""),
         "line 2, column 21: an entry's rx is 9 bytes; it takes 1 to 8"},
        {entry_with("\"7E8\"", "\"7e8\""),
         "line 2, column 21: an entry's rx '7e8' isn't an ID in hex: 0-9 and "
         "A-F"},
        {entry_with("\"7E8\"", "\"0x7E8\""),
         "line 2, column 21: an entry's rx '0x7E8' isn't an ID in hex: 0-9 "
         "and A-F"},
        {entry_with("\"7E8\"", "\"07E8\""),
         "line 2, column 21: an entry's rx '07E8' isn't an ID from 1 to "
         "1FFFFFFF without leading zeros"},
        {entry_with("\"7E8\"", "\"0\""),
         "line 2, column 21: an entry's rx '0' isn't an ID from 1 to 1FFFFFFF "
         "without leading zeros"},
        {entry_with("\"7E8\"", "\"20000000\""),
         "line 2, column 21: an entry's rx '20000000' isn't an ID from 1 to "
         "1FFFFFFF without leading zeros"},
    });
    EXPECT_EQ(refusal(entry_with("\"7E8\"", "\"1FFFFFFF\"")), "");
    EXPECT_EQ(refusal(entry_with("\"7E8\"", "\"1\"")), "");
}

/* A level is a seed request's sub-function inside the ceiling; a count
 * and each second have the file's bounds */
TEST(SecurityStoreTest, AnEntrysLevelCountAndSecondsHaveTheirBounds)
{
    expect_refused({
        {entry_with("\"level\": 1,", "\"level\": 2,"),
         "line 2, column 37: an entry's level isn't a seed request's "
         "sub-function: odd, 1 to 125, never 95"},
        {entry_with("\"level\": 1,", "\"level\": 95,"),
         "line 2, column 37: an entry's level isn't a seed request's "
         "sub-function: odd, 1 to 125, never 95"},
        {entry_with("\"level\": 1,", "\"level\": 127,"),
         "line 2, column 37: an entry's level isn't a seed request's "
         "sub-function: odd, 1 to 125, never 95"},
        {entry_with("\"level\": 1,", "\"level\": 256,"),
         "line 2, column 37: an entry's level takes a whole number from 0 to "
         "255"},
        {entry_with("\"level\": 1,", "\"level\": 1.0,"),
         "line 2, column 37: an entry's level takes a whole number from 0 to "
         "255"},
        {entry_with("\"failed\": 2,", "\"failed\": -1,"),
         "line 2, column 50: an entry's failed takes a whole number from 0 to "
         "2147483647"},
        {entry_with("\"failed\": 2,", "\"failed\": 2147483648,"),
         "line 2, column 50: an entry's failed takes a whole number from 0 to "
         "2147483647"},
        {entry_with("1795132800", "-1"),
         "line 2, column 65: an entry's last_utc takes a whole number from 0 "
         "to 253402300799"},
        {entry_with("1795132800", "253402300800"),
         "line 2, column 65: an entry's last_utc takes a whole number from 0 "
         "to 253402300799"},
        {entry_with("\"pending_utc\": 0", "\"pending_utc\": true"),
         "line 2, column 92: an entry's pending_utc is true or false, not a "
         "number"},
        {entry_with("\"pending_utc\": 0", "\"pending_utc\": 253402300800"),
         "line 2, column 92: an entry's pending_utc takes a whole number from "
         "0 to 253402300799"},
    });
    EXPECT_EQ(refusal(entry_with("\"level\": 1,", "\"level\": 125,")), "");
}

/* The reader's bounds: a list one past the store's most, a file one byte
 * past its longest, a value nested past the file's own depth */
TEST(SecurityStoreTest, AStorePastItsBoundsIsRefused)
{
    std::string list;
    for (size_t k = 0; k <= kStoreMaxEntries; ++k) {
        char one[160];
        std::snprintf(one, sizeof one,
                      "%s{\"rx\": \"%X\", \"level\": 1, \"failed\": 1, "
                      "\"last_utc\": 1, \"pending_utc\": 0, \"vehicle\": "
                      "\"0123456789ABCDEF\"}",
                      k == 0 ? "" : ",", static_cast<unsigned>(0x100 + k));
        list += one;
    }
    EXPECT_EQ(refusal(with_entries(list)),
              "line 2, column 13: \"entries\" lists 257, more than 256");
    const size_t cut = list.rfind(",{");
    EXPECT_EQ(refusal(with_entries(list.substr(0, cut))), "") << "256 read";
    std::string longest = with_entries(kEntry);
    longest.append(kStoreMaxBytes - longest.size(), ' ');
    EXPECT_EQ(refusal(longest), "") << "the longest file reads";
    EXPECT_EQ(refusal(longest + " "),
              "the input is 65537 bytes, past the limit of 65536");
    Folder f;
    f.write(longest);
    store s;
    std::string why;
    EXPECT_EQ(store_load(f.file(), &s, &why), store_io::ok)
        << "and loads from a file of that length: " << why;
    EXPECT_EQ(s.entries.size(), 1u);
    EXPECT_EQ(refusal(with_entries("{\"rx\": []}")),
              "line 2, column 21: nested deeper than 3 arrays and objects")
        << "a list where a store holds a value";
}

/* A vehicle is 16 hex digits, each one written: one spelling for a number */
TEST(SecurityStoreTest, AnEntrysVehicleIsSixteenHexDigits)
{
    const char* const was = "\"0123456789ABCDEF\"";
    expect_refused({
        {entry_with(was, "5"),
         "line 2, column 106: an entry's vehicle is a number, not a string"},
        {entry_with(was, "\"\""),
         "line 2, column 106: an entry's vehicle '' isn't 16 hex digits: 0-9 "
         "and A-F"},
        {entry_with(was, "\"0123456789ABCDE\""),
         "line 2, column 106: an entry's vehicle '0123456789ABCDE' isn't 16 "
         "hex digits: 0-9 and A-F"},
        {entry_with(was, "\"0123456789ABCDEF0\""),
         "line 2, column 106: an entry's vehicle '0123456789ABCDEF0' isn't 16 "
         "hex digits: 0-9 and A-F"},
        {entry_with(was, "\"0123456789abcdef\""),
         "line 2, column 106: an entry's vehicle '0123456789abcdef' isn't 16 "
         "hex digits: 0-9 and A-F"},
        {entry_with(was, "\"0123456789ABCDEG\""),
         "line 2, column 106: an entry's vehicle '0123456789ABCDEG' isn't 16 "
         "hex digits: 0-9 and A-F"},
    });
    EXPECT_EQ(refusal(entry_with(was, "\"0000000000000000\"")), "");
    EXPECT_EQ(refusal(entry_with(was, "\"FFFFFFFFFFFFFFFF\"")), "");
    store s;
    std::string why;
    const std::string top = entry_with(was, "\"FFFFFFFFFFFFFFFF\"");
    ASSERT_EQ(store_read(top.data(), top.size(), &s, &why), store_io::ok);
    ASSERT_EQ(s.entries.size(), 1u);
    EXPECT_EQ(s.entries[0].vehicle, UINT64_MAX);
    EXPECT_EQ(store_find(s, kCar, kRx, 0x01), nullptr) << "another car's";
    EXPECT_NE(store_text(s).find("\"vehicle\": \"FFFFFFFFFFFFFFFF\""),
              std::string::npos);
}

/* The number a file keeps for a vehicle is 64 bits of its identity's
 * SHA-256 (the two the standard's own examples give), and the identity's
 * text reaches no file */
TEST(SecurityStoreTest, AVehiclesNumberIsItsIdentitysHash)
{
    EXPECT_EQ(store_vehicle_of("abc"), 0xBA7816BF8F01CFEAu);
    EXPECT_EQ(store_vehicle_of(""), 0xE3B0C44298FC1C14u);
    EXPECT_EQ(store_vehicle_of("the blue car"),
              store_vehicle_of("the blue car"));
    EXPECT_NE(store_vehicle_of("the blue car"),
              store_vehicle_of("the blue cab"));
    store s;
    store_entry* e =
        store_entry_for(s, store_vehicle_of("the blue car"), kRx, 1);
    ASSERT_NE(e, nullptr);
    e->failed = 1;
    const std::string text = store_text(s);
    EXPECT_EQ(text.find("blue"), std::string::npos) << text;
    EXPECT_EQ(refusal(text), "") << "and what it writes reads";
}
