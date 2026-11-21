// File/layer: tests/test_obd2_identification.cpp | GTest, UNIT tier, headless, all
//             platforms (pure data + decoder + request engine: no
//             transport)
// Testing: UNIT (test_obd2): this file's suites

#include "test_obd2_support.h"

/* ---------- identification (ADR-0021) ---------- */

namespace {

/* Sends channel c's one-shot, which must be identification item i of
 * ECUIO_IDENT_REQUESTS, at now. */
void send_ident_request(ecuio_request_engine* e, uint32_t c, uint32_t i,
                        int64_t now)
{
    const ecuio_ident_request& want = ECUIO_IDENT_REQUESTS[i];
    const int32_t r = ecuio_engine_due(e, c, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE) << "item " << i << " " << want.name;
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    ASSERT_EQ(q.len, want.len) << want.name;
    EXPECT_EQ(memcmp(q.bytes, want.bytes, want.len), 0) << want.name;
    ASSERT_TRUE(e->chans[c].identifying);
    EXPECT_EQ(e->chans[c].ident_index, i);
    ecuio_engine_sent(e, c, r, true, now);
}

/* The ECU's reply to item i: its positive header (49 <item> 01, or
 * 62 <DID>), then data. */
ecuio_reply answer_ident(ecuio_request_engine* e, uint32_t c, uint32_t i,
                         const uint8_t* data, uint16_t n, int64_t now)
{
    const ecuio_ident_request& q = ECUIO_IDENT_REQUESTS[i];
    uint8_t reply[128] = {};
    reply[0] = static_cast<uint8_t>(q.bytes[0] + 0x40);
    reply[1] = q.bytes[1];
    reply[2] = (q.len == 2) ? 0x01 : q.bytes[2];
    memcpy(reply + 3, data, n);
    return ecuio_engine_on_frame(e, c, reply, static_cast<uint16_t>(3 + n),
                                 now, nullptr);
}

} // namespace

/* Identification takes turns with the rotation, ahead of the scan: a
 * rotation request, an item, and so on. A positive reply's data becomes
 * the item's text (ASCII, NUL-padded items joined, or hex), a negative
 * one its code, a timeout leaves it unanswered until a rescan asks it
 * again. Nothing it learns enters the rotation, and the report is
 * complete once every item has an outcome. */
TEST(RequestEngineTest, IdentificationTakesTurnsAndKeepsWhatTheEcuSaid) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t pid_0c[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, pid_0c, 2), ecuio_engine_add_result::added);
    ASSERT_TRUE(ecuio_engine_scan_add(&e, 0, 0x1E20));
    ecuio_engine_identify(&e);
    EXPECT_EQ(e.chans[0].ident_count, ECUIO_IDENT_COUNT);
    EXPECT_EQ(e.chans[0].ident_pending, ECUIO_IDENT_COUNT);
    ecuio_engine_identify(&e);   /* idempotent */
    EXPECT_EQ(e.chans[0].ident_pending, ECUIO_IDENT_COUNT);
    EXPECT_STREQ(ECUIO_IDENT_REQUESTS[0].name, "VIN");

    int64_t now = 0;
    const uint8_t rpm[4] = {0x41, 0x0C, 0x1F, 0x40};
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));   /* the rotation first */
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 0, now));   /* 09 02 */
    const char vin[] = "FAKEJ2534VIN00001";
    EXPECT_EQ(answer_ident(&e, 0, 0, reinterpret_cast<const uint8_t*>(vin), 17,
                           now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].ident[0].outcome, ECUIO_IDENTITY_ANSWERED);
    EXPECT_STREQ(e.chans[0].ident[0].text, vin);
    EXPECT_EQ(e.chans[0].ident[0].text_length, 17u);
    EXPECT_EQ(e.chans[0].ident_generation, 1u);
    EXPECT_EQ(e.count, 1u) << "nothing joins the rotation";

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));   /* the rotation's turn */
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 1, now));   /* 09 04 */
    const uint8_t two_calids[16] = {'C', 'A', 'L', '-', 'A', 0, 0, 0,
                                    'C', 'A', 'L', '-', 'B', 0, 0, 0};
    EXPECT_EQ(answer_ident(&e, 0, 1, two_calids, sizeof(two_calids), now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_STREQ(e.chans[0].ident[1].text, "CAL-A, CAL-B");

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 2, now));   /* 09 06 */
    const uint8_t cvn[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(answer_ident(&e, 0, 2, cvn, sizeof(cvn), now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_STREQ(e.chans[0].ident[2].text, "DE AD BE EF");

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 3, now));   /* 09 0A */
    EXPECT_EQ(answer_negative(&e, 0, 0x09, 0x12, now + 4 * kMs),
              ecuio_reply::negative);
    EXPECT_EQ(e.chans[0].ident[3].outcome, ECUIO_IDENTITY_REFUSED);
    EXPECT_EQ(e.chans[0].ident[3].nrc, 0x12u);
    EXPECT_EQ(e.chans[0].ident[3].text[0], '\0');

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 4, now));   /* 22 F1 87 */
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), ECUIO_ENGINE_WAIT_US);
    now += ECUIO_ENGINE_WAIT_US;
    EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
    EXPECT_EQ(e.chans[0].ident[4].outcome, ECUIO_IDENTITY_UNANSWERED);

    /* The rest are refused; the probe is off, so nothing is dropped. */
    for (uint32_t i = 5; i < ECUIO_IDENT_COUNT; ++i) {
        now += ECUIO_ENGINE_GAP_US;
        ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
        EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs,
                                        nullptr),
                  ecuio_reply::positive);
        now += ECUIO_ENGINE_GAP_US;
        ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, i, now));
        EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
                  ecuio_reply::negative);
        EXPECT_EQ(e.chans[0].ident[i].outcome, ECUIO_IDENTITY_REFUSED);
    }
    EXPECT_EQ(e.chans[0].requests, 1u);
    EXPECT_EQ(e.chans[0].ident_pending, 0u);

    ecuio_channel_identity_t id;
    ecuio_engine_identity(&e, 0, &id);
    EXPECT_EQ(id.flags, ECUIO_IDENTITY_COMPLETE | ECUIO_IDENTITY_VIN);
    EXPECT_EQ(id.items, ECUIO_IDENT_COUNT);
    EXPECT_EQ(id.generation, ECUIO_IDENT_COUNT);
    EXPECT_STREQ(id.list[0].text, vin);
    EXPECT_EQ(id.list[12].request[2], 0x95u);

    /* The scan waited behind identification; now it has the turn. */
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_scan_request(&e, 0, 0x1E20, now));
    EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
              ecuio_reply::negative);

    /* A rescan asks the unanswered item again, and only it. */
    ecuio_engine_rescan(&e);
    EXPECT_EQ(e.chans[0].ident_pending, 1u);
    EXPECT_EQ(e.chans[0].ident[4].outcome, ECUIO_IDENTITY_UNASKED);
    ecuio_engine_identity(&e, 0, &id);
    EXPECT_EQ(id.flags, ECUIO_IDENTITY_VIN)
        << "an unasked item isn't complete; the VIN stays in";
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now));
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, rpm, sizeof(rpm), now + 4 * kMs, nullptr),
              ecuio_reply::positive);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 4, now));
    const char part[] = "FAKE-PART-0001";
    EXPECT_EQ(answer_ident(&e, 0, 4, reinterpret_cast<const uint8_t*>(part), 14,
                           now + 4 * kMs),
              ecuio_reply::positive);
    ecuio_engine_identity(&e, 0, &id);
    EXPECT_EQ(id.flags, ECUIO_IDENTITY_COMPLETE | ECUIO_IDENTITY_VIN);
    EXPECT_EQ(id.generation, ECUIO_IDENT_COUNT + 1);
    EXPECT_STREQ(id.list[4].text, part);
    EXPECT_EQ(e.count, 1u);
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_rotation(&e, 0, 0, now)) << "only the rotation is left";
}

/* A channel with nothing to rotate identifies every turn, in list order,
 * and has nothing to send once the list is asked. A channel never given
 * the list reports no items and never reads complete. */
TEST(RequestEngineTest, IdentificationAloneRunsEveryTurnAndThenRests) {
    ecuio_request_engine e;
    ecuio_engine_init(&e, 2);
    ecuio_channel_identity_t id;
    ecuio_engine_identity(&e, 0, &id);
    EXPECT_EQ(id.items, 0u);
    EXPECT_EQ(id.flags, 0u);
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), INT64_MAX);

    ecuio_engine_identify(&e);
    EXPECT_EQ(e.chans[1].ident_count, ECUIO_IDENT_COUNT) << "every channel";
    EXPECT_EQ(ecuio_engine_next_event_us(&e, 0), 0);
    int64_t now = 0;
    for (uint32_t i = 0; i < ECUIO_IDENT_COUNT; ++i) {
        ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, i, now));
        if (i % 2 == 0) {
            now += ECUIO_ENGINE_WAIT_US;
            EXPECT_TRUE(ecuio_engine_expire(&e, 0, now));
        } else {
            EXPECT_EQ(answer_negative(&e, 0, ECUIO_IDENT_REQUESTS[i].bytes[0], 0x31,
                                      now + 4 * kMs),
                      ecuio_reply::negative);
            now += ECUIO_ENGINE_GAP_US;
        }
    }
    EXPECT_EQ(ecuio_engine_due(&e, 0, now), ECUIO_ENGINE_NONE);
    ecuio_engine_identity(&e, 0, &id);
    EXPECT_EQ(id.flags, ECUIO_IDENTITY_COMPLETE) << "unanswered counts as settled";
    EXPECT_EQ(id.list[0].outcome, ECUIO_IDENTITY_UNANSWERED);
    EXPECT_EQ(id.list[1].outcome, ECUIO_IDENTITY_REFUSED);
    /* Channel 1 still has its whole list to ask. */
    EXPECT_EQ(ecuio_engine_next_event_us(&e, now), 0);
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 1, 0, now));
}

/* ADR-0027 section 3: the report flags the VIN as soon as it is in,
 * long before the list is complete: 09 02's answer at once; F190's only
 * once 09 02 has an outcome, since an unasked 09 02 could still answer a
 * VIN of its own. A rescan that asks 09 02 again takes the flag back
 * until it settles. */
TEST(RequestEngineTest, TheVinIsFlaggedOnceItIsInWhateverIsStillOut)
{
    const char vin[] = "FAKEJ2534VIN00001";
    const char other[] = "1FAFAKE10M0000001";
    ecuio_channel_identity_t id;
    {
        ecuio_request_engine e;
        ecuio_engine_init(&e, 1);
        ecuio_engine_identify(&e);
        ecuio_engine_identity(&e, 0, &id);
        EXPECT_EQ(id.flags, 0u);
        int64_t now = 0;
        ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 0, now)); /* 09 02 */
        EXPECT_EQ(answer_ident(&e, 0, 0, reinterpret_cast<const uint8_t*>(vin),
                               17, now + 4 * kMs),
                  ecuio_reply::positive);
        now += ECUIO_ENGINE_GAP_US;
        ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 1, now)); /* out */
        ecuio_engine_identity(&e, 0, &id);
        EXPECT_EQ(id.flags, ECUIO_IDENTITY_VIN) << "in, the list not complete";
        char text[ECUIO_VIN_TEXT_LEN];
        ASSERT_EQ(ecuio_identity_vin(&id, text, sizeof text), ECUIO_OK);
        EXPECT_STREQ(text, vin);
    }
    {
        ecuio_request_engine e;
        ecuio_engine_init(&e, 1);
        ecuio_engine_identify(&e);
        int64_t now = 0;
        ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 0, now)); /* 09 02 */
        now += ECUIO_ENGINE_WAIT_US;
        ASSERT_TRUE(ecuio_engine_expire(&e, 0, now)); /* unanswered */
        for (uint32_t i = 1; i < 9; ++i) {
            ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, i, now));
            EXPECT_EQ(answer_negative(&e, 0, ECUIO_IDENT_REQUESTS[i].bytes[0],
                                      0x31, now + 4 * kMs),
                      ecuio_reply::negative);
            now += ECUIO_ENGINE_GAP_US;
        }
        ecuio_engine_identity(&e, 0, &id);
        EXPECT_EQ(id.flags, 0u) << "no VIN answered yet";
        ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 9, now)); /* F190 */
        EXPECT_EQ(answer_ident(&e, 0, 9, reinterpret_cast<const uint8_t*>(vin),
                               17, now + 4 * kMs),
                  ecuio_reply::positive);
        now += ECUIO_ENGINE_GAP_US;
        ecuio_engine_identity(&e, 0, &id);
        EXPECT_EQ(id.flags, ECUIO_IDENTITY_VIN) << "09 02 has its outcome";
        for (uint32_t i = 10; i < ECUIO_IDENT_COUNT; ++i) {
            ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, i, now));
            EXPECT_EQ(answer_negative(&e, 0, 0x22, 0x31, now + 4 * kMs),
                      ecuio_reply::negative);
            now += ECUIO_ENGINE_GAP_US;
        }
        ecuio_engine_identity(&e, 0, &id);
        EXPECT_EQ(id.flags, ECUIO_IDENTITY_COMPLETE | ECUIO_IDENTITY_VIN);

        ecuio_engine_rescan(&e); /* 09 02 asked again, and only it */
        ecuio_engine_identity(&e, 0, &id);
        EXPECT_EQ(id.list[0].outcome, ECUIO_IDENTITY_UNASKED);
        EXPECT_EQ(id.flags & ECUIO_IDENTITY_VIN, 0u)
            << "09 02 could still answer one of its own";
        ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 0, now));
        EXPECT_EQ(answer_ident(&e, 0, 0,
                               reinterpret_cast<const uint8_t*>(other), 17,
                               now + 4 * kMs),
                  ecuio_reply::positive);
        ecuio_engine_identity(&e, 0, &id);
        EXPECT_NE(id.flags & ECUIO_IDENTITY_VIN, 0u);
        char text[ECUIO_VIN_TEXT_LEN];
        ASSERT_EQ(ecuio_identity_vin(&id, text, sizeof text), ECUIO_OK);
        EXPECT_STREQ(text, other) << "09 02's comes first";
    }
}

namespace {

ecuio_identity_item_t vin_item(uint8_t service, uint8_t key_hi, uint8_t key_lo,
                               uint8_t outcome, const char* text)
{
    ecuio_identity_item_t it{};
    it.request[0] = service;
    it.request[1] = key_hi;
    it.request[2] = key_lo;
    it.length = service == 0x09 ? 2 : 3;
    it.outcome = outcome;
    const size_t n = strlen(text);
    memcpy(it.text, text, n);
    it.text_length = static_cast<uint8_t>(n);
    return it;
}

} // namespace

/* The pick behind the flag and ecuio_identity_vin: an item's text kept to
 * its letters and digits, upper-cased, a VIN only at exactly 17; 09 02's
 * before F190's; F190's in only beside a 09 02 with an outcome, or none
 * in the list. */
TEST(RequestEngineTest, AReportsVinIsNineOhTwosThenF190sOnceItIsIn)
{
    const auto pick = [](std::initializer_list<ecuio_identity_item_t> items) {
        ecuio_channel_identity_t id{};
        for (const ecuio_identity_item_t& it : items) {
            id.list[id.items++] = it;
        }
        return ecuio_identity_vin_of(id);
    };
    const uint8_t A = ECUIO_IDENTITY_ANSWERED;
    const char vin[] = "1FAFAKE10M0000001";
    const char f190[] = "FAKEJ2534VIN00001";

    ecuio_ident_vin v =
        pick({vin_item(0x09, 0x02, 0, A, "1fa fake10m-0000001")});
    EXPECT_TRUE(v.in);
    EXPECT_STREQ(v.text, vin) << "letters and digits, upper case";

    v = pick({vin_item(0x09, 0x02, 0, A, "1FAFAKE10M00000012"),
              vin_item(0x22, 0xF1, 0x90, A, f190)});
    EXPECT_TRUE(v.in) << "eighteen is no VIN; 09 02 has its outcome";
    EXPECT_STREQ(v.text, f190);

    v = pick({vin_item(0x09, 0x02, 0, A, "1FAFAKE10M000000"),
              vin_item(0x22, 0xF1, 0x90, A, f190)});
    EXPECT_STREQ(v.text, f190) << "sixteen is no VIN either";

    v = pick({vin_item(0x09, 0x02, 0, ECUIO_IDENTITY_UNASKED, ""),
              vin_item(0x22, 0xF1, 0x90, A, f190)});
    EXPECT_FALSE(v.in);
    EXPECT_STREQ(v.text, f190) << "ecuio_identity_vin still gives it";

    v = pick({vin_item(0x09, 0x02, 0, ECUIO_IDENTITY_REFUSED, vin),
              vin_item(0x22, 0xF1, 0x90, A, f190)});
    EXPECT_TRUE(v.in);
    EXPECT_STREQ(v.text, f190) << "a refusal's text isn't an answer";

    v = pick({vin_item(0x22, 0xF1, 0x90, A, f190)});
    EXPECT_TRUE(v.in) << "no 09 02 in the list";

    v = pick(
        {vin_item(0x09, 0x04, 0, A, vin), vin_item(0x22, 0xF1, 0x91, A, vin)});
    EXPECT_FALSE(v.in) << "only 09 02 and F190 carry a VIN";
    EXPECT_STREQ(v.text, "");

    v = pick({vin_item(0x09, 0x02, 0, A, vin),
              vin_item(0x22, 0xF1, 0x90, ECUIO_IDENTITY_UNASKED, "")});
    EXPECT_TRUE(v.in) << "09 02's needs nothing of F190";
}

namespace {

/* A connect whose walk and first rotation request went unanswered, and
 * whose next turn has the VIN item in the one-shot, not yet sent: the
 * state a recovery's probe can meet. */
void identify_after_a_silent_walk(ecuio_request_engine* e, int64_t* now)
{
    ecuio_engine_init(e, 1);
    const uint8_t pid_0c[2] = {0x01, 0x0C};
    ASSERT_EQ(ecuio_engine_add(e, 0, pid_0c, 2), ecuio_engine_add_result::added);
    ecuio_engine_identify(e);
    ecuio_engine_probe(e);
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(e, 0, 0x00, *now));
    *now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(e, 0, *now));   /* the walk ends unjudged */
    ASSERT_NO_FATAL_FAILURE(send_rotation(e, 0, 0, *now));
    *now += ECUIO_ENGINE_WAIT_US;
    ASSERT_TRUE(ecuio_engine_expire(e, 0, *now));
}

const char kVin[] = "FAKEJ2534VIN00001";

} // namespace

/* A recovery restarts the walk while an identification item is out: the
 * item keeps the one-shot, its reply settles it, and the walk follows.
 * The recovery's rescan has already run, so an item lost here would stay
 * unanswered until the next one. */
TEST(RequestEngineTest, RecoveryWalkWaitsForTheItemOut) {
    ecuio_request_engine e;
    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(identify_after_a_silent_walk(&e, &now));
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 0, now));   /* 09 02 */

    ecuio_engine_probe(&e);   /* the driver's recovery calls */
    ecuio_engine_rescan(&e);
    EXPECT_TRUE(e.chans[0].walking);
    EXPECT_EQ(answer_ident(&e, 0, 0, reinterpret_cast<const uint8_t*>(kVin), 17,
                           now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_EQ(e.chans[0].ident[0].outcome, ECUIO_IDENTITY_ANSWERED);
    EXPECT_STREQ(e.chans[0].ident[0].text, kVin);

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
    EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 1u << (32 - 0x0C), now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_TRUE(e.chans[0].walked);
    EXPECT_EQ(e.chans[0].requests, 1u) << "0x0C is supported";
}

/* A recovery restarts the walk while an identification item waits in
 * the one-shot unsent (its write failed): the item goes out first, so the
 * walk's bitmap reply can never be taken for the item's answer. */
TEST(RequestEngineTest, RecoveryWalkWaitsForAnUnsentItem) {
    ecuio_request_engine e;
    int64_t now = 0;
    ASSERT_NO_FATAL_FAILURE(identify_after_a_silent_walk(&e, &now));
    const int32_t r = ecuio_engine_due(&e, 0, now);
    ASSERT_EQ(r, ECUIO_ENGINE_ONCE);
    ASSERT_TRUE(e.chans[0].identifying);
    ecuio_engine_sent(&e, 0, r, false, now);   /* the write failed */

    ecuio_engine_probe(&e);   /* another channel recovered the link */
    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_ident_request(&e, 0, 0, now));
    EXPECT_EQ(answer_ident(&e, 0, 0, reinterpret_cast<const uint8_t*>(kVin), 17,
                           now + 4 * kMs),
              ecuio_reply::positive);

    now += ECUIO_ENGINE_GAP_US;
    ASSERT_NO_FATAL_FAILURE(send_bitmap_request(&e, 0, 0x00, now));
    EXPECT_EQ(answer_bitmap(&e, 0, 0x00, 1u << (32 - 0x0C), now + 4 * kMs),
              ecuio_reply::positive);
    EXPECT_STREQ(e.chans[0].ident[0].text, kVin) << "the bitmap isn't the VIN";
    EXPECT_TRUE(e.chans[0].walked);
}

/* The report's text: ASCII when the bytes are printable or padding, the
 * padded items joined and trailing blanks dropped; hex pairs otherwise;
 * cut to the field at a whole pair. */
TEST(RequestEngineTest, IdentityTextIsAsciiOrHexAndCutToTheField) {
    char out[ECUIO_IDENTITY_TEXT_LEN];
    const auto text = [&out](std::initializer_list<uint8_t> bytes,
                             uint32_t cap = ECUIO_IDENTITY_TEXT_LEN) {
        uint8_t data[80] = {};
        uint32_t n = 0;
        for (uint8_t b : bytes) {
            data[n++] = b;
        }
        const uint8_t len = ecuio_identity_format(data, n, out, cap);
        EXPECT_EQ(len, strlen(out));
        return std::string(out);
    };
    EXPECT_EQ(text({'A', 'B', 'C'}), "ABC");
    EXPECT_EQ(text({'A', 'B', 'C', 0, 0, 0}), "ABC");
    EXPECT_EQ(text({'A', 'B', ' ', ' ', 0xFF}), "AB");
    EXPECT_EQ(text({'A', 0, 0, 'B', 0xFF, 'C'}), "A, B, C");
    EXPECT_EQ(text({' ', ' '}), "");
    EXPECT_EQ(text({0x00, 0x01}), "00 01");
    EXPECT_EQ(text({0xFF, 0xFF}), "FF FF") << "padding alone isn't text";
    EXPECT_EQ(text({'A', 0x7F}), "41 7F");
    EXPECT_EQ(text({}), "");

    uint8_t many[70];
    memset(many, 'A', sizeof(many));
    EXPECT_EQ(ecuio_identity_format(many, sizeof(many), out, sizeof(out)), 63u);
    EXPECT_EQ(strlen(out), 63u);
    memset(many, 0x10, sizeof(many));
    EXPECT_EQ(ecuio_identity_format(many, 30, out, sizeof(out)), 62u)
        << "21 pairs fit in 63 characters";
    EXPECT_EQ(out[61], '0');
    EXPECT_EQ(text({'A', 'B', 'C', 'D'}, 4), "ABC");
    EXPECT_EQ(text({0x12, 0x34}, 4), "12") << "the second pair needs three more";
    EXPECT_EQ(text({'A', 0, 'B'}, 4), "A") << "the separator needs two";
}

TEST(RequestEngineTest, IdentityRequestsAreNamed) {
    const uint8_t vin[2] = {0x09, 0x02};
    const uint8_t supplier_version[3] = {0x22, 0xF1, 0x95};
    const uint8_t other[3] = {0x22, 0x12, 0x34};
    EXPECT_STREQ(ecuio_identity_request_name(vin, 2), "VIN");
    EXPECT_STREQ(ecuio_identity_request_name(supplier_version, 3),
                 "supplier software version");
    EXPECT_STREQ(ecuio_identity_request_name(other, 3), "");
    EXPECT_STREQ(ecuio_identity_request_name(vin, 3), "") << "the length counts";
    EXPECT_STREQ(ecuio_identity_request_name(nullptr, 2), "");
}
