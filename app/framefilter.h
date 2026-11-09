// File/layer: app/framefilter.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: FrameFilterTest, BridgeTest.FrameFilter* (test_bridge)
//
// The Log page's filter: which of the frames the table holds it shows,
// by the columns an engineer reads a bus by (the ID, the data bytes, the
// flags, the direction). It chooses rows to show, as a search does; it
// reads no field of a payload, so decoding stays in core (rule 2).
//
//   ID     hex IDs, ranges and IDs under a mask, commas between, "0x"
//          optional: "7E8, 7E0-7EF, 0x18DAF110, 7E0:7F8". A mask is
//          candump's can_id:can_mask, the ID's bits where the mask has
//          ones. An ID (or a mask) past 0x7FF matches only an extended
//          (29-bit) frame.
//   Data   hex bytes, "??" for any byte, found anywhere in the payload;
//          "^" first anchors them at byte 0: "^62 F1 ??".
//   Length data lengths in bytes and ranges, decimal, commas between:
//          "8", "1-7, 12, 64".
//   Time   the table's seconds, one range, both ends in it: "12.5-30",
//          "12.5-" (from then on) or "-30" (up to then).
//   Direction  every frame, the received ones, or the sent ones (an
//          echo of the session's own transmit, ECUIO_FRAME_FLAG_TX_ECHO).
//   Flags  any, or one: extended, standard, CAN FD, reassembled,
//          truncated, after a loss (overflow), error.

#pragma once

#include <QString>

#include <cstdint>
#include <vector>

class FrameFilter
{
public:
    enum Direction { AnyDirection = 0, Received = 1, Sent = 2 };
    enum Flag {
        AnyFlag = 0,
        Extended = 1,
        Standard = 2,
        Fd = 3,
        Reassembled = 4,
        Truncated = 5,
        Overflow = 6,
        Error = 7
    };

    // first to last; or, masked, the IDs whose bits under mask are
    // first's (last is first)
    struct IdRange {
        uint32_t first = 0;
        uint32_t last = 0;
        bool masked = false;
        uint32_t mask = 0;
        bool operator==(const IdRange& o) const
        {
            return first == o.first && last == o.last && masked == o.masked &&
                   mask == o.mask;
        }
    };
    struct LengthRange {
        int first = 0;
        int last = 0;
        bool operator==(const LengthRange& o) const
        {
            return first == o.first && last == o.last;
        }
    };
    // Session time in microseconds, both ends in the range
    struct TimeRange {
        bool set = false;
        uint64_t first = 0;
        uint64_t last = UINT64_MAX;
        bool operator==(const TimeRange& o) const
        {
            return set == o.set && first == o.first && last == o.last;
        }
    };

    // The ID field's text into ranges; "" when it parses (empty is any
    // ID), else why not, naming the item
    static QString parseIds(const QString& text, std::vector<IdRange>* out);
    // The data field's text into bytes (-1 for "??"); "" when it parses
    // (empty is any payload), else why not
    static QString parseData(const QString& text, std::vector<int>* out,
                             bool* anchored);
    // The length field's text into ranges; "" when it parses (empty is
    // any length), else why not, naming the item
    static QString parseLengths(const QString& text,
                                std::vector<LengthRange>* out);
    // The time field's text into a range; "" when it parses (empty is
    // any time, not set), else why not
    static QString parseTimes(const QString& text, TimeRange* out);

    std::vector<IdRange> ids;
    std::vector<int> data;
    bool anchored = false;
    int direction = AnyDirection;
    int flag = AnyFlag;
    std::vector<LengthRange> lengths;
    TimeRange time;

    // How many fields are in force: 0 is no filter, every frame shown
    int terms() const;
    bool active() const { return terms() > 0; }
    // A frame as the ring holds it: its ID, its ECUIO_FRAME_FLAG_*, its
    // session time and its payload
    bool matches(uint32_t id, uint32_t flags, uint64_t timestampUs,
                 const uint8_t* payload, int length) const;
};
