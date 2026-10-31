// File/layer: core/src/obd2_reply.h | C++17, Qt-free, ALL platforms (internal)
// Testing: UNIT: tests/test_obd2.cpp (Obd2ReplyTest: each judgment of
//          ecuio_obd2_split; Obd2TableTest: ECUIO_OBD2_PID_BYTES against
//          tests/fixtures/obd2/service01.csv's bytes column), and through
//          a session's decode in tests/test_core.cpp (CoreTest.APacked*)
//
// A Service 01 positive reply's records (ADR-0039 Appendix A): J1979
// lets one request carry up to six PIDs, and the ECU answers
// 41 PID data PID data ..., each record as long as its PID's data. Core's
// decode splits such a reply into its records and decodes each as if it
// were its own reply, so every def's key and start byte stay as they are
// and the wire stays as the ECU sent it. Pure: no session, no allocation.

#pragma once

#include <cstdint>

/* The data bytes J1979 gives each Service 01 PID's record, 0 for a PID
 * it doesn't define here. Typed from tests/fixtures/obd2/service01.csv
 * and checked against it by Obd2TableTest, never generated from it. */
constexpr uint8_t ECUIO_OBD2_PID_BYTES[256] = {
    /* 00 */ 4, 4, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1,
    /* 10 */ 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2,
    /* 20 */ 4, 2, 2, 2, 4, 4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 1,
    /* 30 */ 1, 2, 2, 1, 4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 2,
    /* 40 */ 4, 4, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 4,
    /* 50 */ 4, 1, 1, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 1,
    /* 60 */ 4, 1, 1, 2, 5, 2, 5, 3, 3, 7, 5, 5, 5, 11, 9, 3,
    /* 70 */ 10, 6, 5, 5, 5, 7, 7, 5, 9, 9, 7, 7, 9, 1, 1, 13,
    /* 80 */ 4, 41, 41, 9, 1, 10, 5, 5, 13, 41, 41, 7, 17, 1, 1, 7,
    /* 90 */ 3, 5, 2, 3, 12, 0, 0, 0, 9, 9, 6, 4, 17, 4, 2, 9,
    /* A0 */ 4, 9, 2, 9, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0,
    /* B0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* C0 */ 4, 0, 0, 2, 8, 4, 7, 2, 1, 0, 0, 0, 0, 0, 0, 0,
    /* D0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* E0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* F0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* The most records one reply is split into: J1979's six, with room for
 * a reply to the walk's bitmaps and an ECU that answers more */
constexpr uint32_t ECUIO_OBD2_MAX_RECORDS = 16;

/* One record: its PID, and its data bytes' place in the reply */
struct ecuio_obd2_record {
    uint8_t pid;
    uint16_t start;   /* the first data byte's index in the reply */
    uint16_t bytes;
};

enum class ecuio_obd2_reply : uint8_t {
    whole,     /* not a Service 01 reply to split: decode it as it came */
    split,     /* two or more records, filling the reply exactly */
    unsplit,   /* longer than its first record, but its records don't
                  split: its first record decodes, the rest don't */
};

/* What reply (len bytes) holds. learned[pid] is a PID's data length as
 * this ECU sends it alone, where that's shorter than J1979's (0 = the
 * table's), and this keeps it: a reply no longer than its first PID's
 * length is that PID's alone (core can't see the request, only the
 * length), so it teaches the length when it's shorter than the table's
 * (the test car's PID 55: bank 1's byte, no bank 3), and the table's
 * length again when it isn't. A longer reply is split by those lengths:
 * each record's PID known and seen once, at most ECUIO_OBD2_MAX_RECORDS,
 * ending exactly at the reply's end; the records go to out (*n of
 * them). A reply that won't split is the first PID's alone when it's
 * no longer than the table's length (a longer one than this ECU sent
 * before), else unsplit. */
inline ecuio_obd2_reply ecuio_obd2_split(const uint8_t* reply, uint16_t len,
                                         uint8_t* learned,
                                         ecuio_obd2_record* out, uint32_t* n)
{
    *n = 0;
    if (len < 3 || reply[0] != 0x41u) {
        return ecuio_obd2_reply::whole;
    }
    const uint8_t first = reply[1];
    const uint16_t table = ECUIO_OBD2_PID_BYTES[first];
    const uint16_t known = learned[first] != 0 ? learned[first] : table;
    const uint16_t data = static_cast<uint16_t>(len - 2);
    const auto alone = [&]() {
        learned[first] = data < table ? static_cast<uint8_t>(data) : 0;
        return ecuio_obd2_reply::whole;
    };
    if (known == 0) {
        return ecuio_obd2_reply::whole;
    }
    if (data <= known) {
        return alone();
    }

    bool seen[256] = {};
    uint32_t count = 0;
    uint32_t at = 1;
    while (at < len) {
        const uint8_t pid = reply[at];
        const uint32_t bytes =
            learned[pid] != 0 ? learned[pid] : ECUIO_OBD2_PID_BYTES[pid];
        if (bytes == 0 || seen[pid] || count == ECUIO_OBD2_MAX_RECORDS ||
            at + 1 + bytes > len) {
            break;
        }
        seen[pid] = true;
        out[count++] = {pid, static_cast<uint16_t>(at + 1),
                        static_cast<uint16_t>(bytes)};
        at += 1 + bytes;
    }
    if (at == len && count >= 2) {
        *n = count;
        return ecuio_obd2_reply::split;
    }
    return data <= table ? alone() : ecuio_obd2_reply::unsplit;
}
