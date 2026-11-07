// File/layer: app/framefilter.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: FrameFilterTest, BridgeTest.FrameFilter* (test_bridge)
//
// The Log page's filter: which of the frames the table holds it shows,
// by the columns an engineer reads a bus by (the ID, the data bytes, the
// flags, the direction). It chooses rows to show, as a search does; it
// reads no field of a payload, so decoding stays in core (rule 2).
//
//   ID     hex IDs and ranges, commas between, "0x" optional:
//          "7E8, 7E0-7EF, 0x18DAF110". An ID past 0x7FF matches only an
//          extended (29-bit) frame.
//   Data   hex bytes, "??" for any byte, found anywhere in the payload;
//          "^" first anchors them at byte 0: "^62 F1 ??".
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

    struct IdRange {
        uint32_t first = 0;
        uint32_t last = 0;
    };

    // The ID field's text into ranges; "" when it parses (empty is any
    // ID), else why not, naming the item
    static QString parseIds(const QString& text, std::vector<IdRange>* out);
    // The data field's text into bytes (-1 for "??"); "" when it parses
    // (empty is any payload), else why not
    static QString parseData(const QString& text, std::vector<int>* out,
                             bool* anchored);

    std::vector<IdRange> ids;
    std::vector<int> data;
    bool anchored = false;
    int direction = AnyDirection;
    int flag = AnyFlag;

    // How many fields are in force: 0 is no filter, every frame shown
    int terms() const;
    bool active() const { return terms() > 0; }
    // A frame as the ring holds it: its ID, its ECUIO_FRAME_FLAG_* and
    // its payload
    bool matches(uint32_t id, uint32_t flags, const uint8_t* payload,
                 int length) const;
};
