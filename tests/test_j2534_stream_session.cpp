// File/layer: tests/test_j2534_stream_session.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534AdoptionTest (test_j2534_stream)

#include "test_j2534_stream_support.h"

#ifdef _WIN32

namespace {

/* ADR-0060: ?session=03 writes 10 03 first, the engine ECU answers D200
 * only in the session it entered, and the close's last write is 10 01 */
TEST_F(J2534AdoptionTest, AnExtendedSessionIsEnteredFirstAndLeftAtClose)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=uds-generic&did=1E1C,D200&session=03"
             "&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
        << ecuio_last_error_detail();
    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for_count(cap, 4, 200);

    bool d200 = false;
    const int n = cap.count.load() < 8 ? cap.count.load() : 8;
    for (int i = 0; i < n; ++i) {
        const unsigned char want[5] = {0x62, 0xD2, 0x00, 0x1A, 0x2C};
        d200 = d200 ||
               (cap.arbitration_ids[i] == 0x7E8u && cap.payload_lens[i] == 5 &&
                memcmp(cap.payloads[i], want, 5) == 0);
    }
    EXPECT_TRUE(d200) << "D200 answers only in the extended session";
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);

    unsigned long count = 0;
    get_write_count(&count);
    ASSERT_GE(count, 2u);
    ASSERT_LE(count, 16u) << "the page keeps the first 16 writes";
    unsigned char w[64] = {};
    unsigned long len = 0, flags = 0;
    get_recorded_write(0, w, &len, &flags);
    const unsigned char first[6] = {0x00, 0x00, 0x07, 0xE0, 0x10, 0x03};
    ASSERT_EQ(len, 6u);
    EXPECT_EQ(memcmp(w, first, sizeof(first)), 0) << "the first write";
    get_recorded_write(count - 1, w, &len, &flags);
    const unsigned char last[6] = {0x00, 0x00, 0x07, 0xE0, 0x10, 0x01};
    ASSERT_EQ(len, 6u);
    EXPECT_EQ(memcmp(w, last, sizeof(last)), 0) << "the close's write";
}

/* Without ?session=03 the engine ECU stays in the default session: D200
 * is refused (7F 22 31), never answered, while 1E1C reads */
TEST_F(J2534AdoptionTest, WithoutASessionD200IsRefused)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=uds-generic&did=1E1C,D200&identify=0"
             "&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
        << ecuio_last_error_detail();
    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for_count(cap, 6, 200);
    const int n = cap.count.load() < 8 ? cap.count.load() : 8;
    ASSERT_GE(n, 3);
    for (int i = 0; i < n; ++i) {
        EXPECT_FALSE(cap.payload_lens[i] >= 3 && cap.payloads[i][0] == 0x62 &&
                     cap.payloads[i][1] == 0xD2)
            << "D200 answered in the default session";
    }
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
    unsigned long count = 0;
    get_write_count(&count);
    for (unsigned long i = 0; i < count && i < 16; ++i) {
        unsigned char w[64] = {};
        unsigned long len = 0, flags = 0;
        get_recorded_write(i, w, &len, &flags);
        EXPECT_FALSE(len == 6u && w[4] == 0x10) << "no session control asked";
    }
}

/* A module that refuses 10 03 (7F 10 12) is read in the default session
 * and gets no 10 01 at the close; the engine ECU's D200 reads too */
TEST_F(J2534AdoptionTest, AModuleThatRefusesTheSessionIsReadInTheDefaultOne)
{
    KnobGuard module("ECUIO_FAKE_MODULE_DIDS", "7E9:D801=00 12");
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=uds-generic&did=D200,7E9:D801&session=03"
             "&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
        << ecuio_last_error_detail();
    StreamCapture cap;
    ASSERT_EQ(ecuio_register_frame_callback(session, capture_cb, &cap),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for_count(cap, 6, 200);

    bool engine = false, module_ok = false;
    const int n = cap.count.load() < 8 ? cap.count.load() : 8;
    for (int i = 0; i < n; ++i) {
        const unsigned char d200[5] = {0x62, 0xD2, 0x00, 0x1A, 0x2C};
        const unsigned char d801[5] = {0x62, 0xD8, 0x01, 0x00, 0x12};
        engine = engine || (cap.arbitration_ids[i] == 0x7E8u &&
                            cap.payload_lens[i] == 5 &&
                            memcmp(cap.payloads[i], d200, 5) == 0);
        module_ok = module_ok || (cap.arbitration_ids[i] == 0x7E9u &&
                                  cap.payload_lens[i] == 5 &&
                                  memcmp(cap.payloads[i], d801, 5) == 0);
    }
    EXPECT_TRUE(engine) << "D200 on 0x7E8";
    EXPECT_TRUE(module_ok) << "D801 on 0x7E9, in the default session";
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);

    unsigned long count = 0;
    get_write_count(&count);
    ASSERT_LE(count, 16u) << "the page keeps the first 16 writes";
    int tried_module = 0, left_engine = 0, left_module = 0;
    for (unsigned long i = 0; i < count; ++i) {
        unsigned char w[64] = {};
        unsigned long len = 0, flags = 0;
        get_recorded_write(i, w, &len, &flags);
        if (len != 6u || w[4] != 0x10) {
            continue;
        }
        const unsigned id = (unsigned{w[2]} << 8) | w[3];
        if (w[5] == 0x03) {
            tried_module += id == 0x7E1u;
        } else if (w[5] == 0x01) {
            left_engine += id == 0x7E0u;
            left_module += id == 0x7E1u;
        }
    }
    EXPECT_GE(tried_module, 1) << "10 03 goes to the module too";
    EXPECT_EQ(left_engine, 1);
    EXPECT_EQ(left_module, 0) << "no 10 01 to a module outside the session";
}

/* A range is scanned once, in order: of 1E18 to 1E20 the fake's ECU
 * knows only 1E1C, which the scan finds and then polls, taking turns
 * with the rest of the scan, while the eight it refuses (7F 22 31) are
 * counted and never asked again. */
TEST_F(J2534AdoptionTest, UdsGenericScanFindsWhatTheEcuAnswers)
{
    char uri[700];
    snprintf(
        uri, sizeof(uri),
        "j2534://%s?profile=uds-generic&did=1E18-1E20&identify=0&diagnose=0",
        FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_signal_set(session)->count, 9u);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    ecuio_channel_support_t s = {};
    EXPECT_TRUE(wait_for_support(session, s, [](const ecuio_channel_support_t& r) {
        return (r.flags & ECUIO_SUPPORT_SCAN_COMPLETE) != 0;
    }, 3000)) << "the scan didn't end (done " << s.scan_done << ")";
    EXPECT_EQ(s.flags, ECUIO_SUPPORT_SCAN_COMPLETE) << "no walk: no Service 01";
    EXPECT_EQ(s.scan_total, 9u);
    EXPECT_EQ(s.scan_found, 1u);
    EXPECT_EQ(s.scan_refused, 8u);
    EXPECT_EQ(s.scan_polled, 1u);
    EXPECT_EQ(s.requests, 1u) << "the found DID, and nothing else, rotates";
    EXPECT_EQ(s.dropped, 0u);

    wait_for_writes(get_write_count, 14, 300);
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0, wflags = 0;
    /* The scan alone until 1E1C is found at write 4; from then on the
     * rotation (1E1C) and the scan's remaining DIDs take turns, and once
     * the scan is over only 1E1C is asked. */
    const uint8_t want[14] = {0x18, 0x19, 0x1A, 0x1B, 0x1C,
                              0x1C, 0x1D, 0x1C, 0x1E, 0x1C, 0x1F, 0x1C, 0x20,
                              0x1C};
    for (unsigned long i = 0; i < 14; ++i) {
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_EQ(wlen, 7u) << "write " << i;
        EXPECT_EQ(wbuf[4], 0x22u) << "write " << i;
        EXPECT_EQ(wbuf[5], 0x1Eu) << "write " << i;
        EXPECT_EQ(wbuf[6], want[i]) << "write " << i;
    }

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* Identification (ADR-0021): with the set's requests, the channel asks
 * the list once, taking turns with the rotation, and the report carries
 * what the fake's ECU said it is: its FAKE VIN, CALID, CVN and name, the
 * text DIDs it knows, and the two DIDs it refuses. ?probe=0 keeps the
 * walk out of the write order. */
TEST_F(J2534AdoptionTest, IdentificationAsksTheListOnceAndReportsIt)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=obd2&probe=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ecuio_channel_identity_t id{};
    ASSERT_EQ(ecuio_session_get_channel_identity(session, 0, &id), ECUIO_OK);
    EXPECT_EQ(id.rx_id, 0x7E8u);
    EXPECT_EQ(id.items, ECUIO_IDENT_COUNT);
    EXPECT_EQ(id.flags, 0u);
    EXPECT_EQ(id.generation, 0u);
    EXPECT_EQ(ecuio_session_get_channel_identity(session, 1, &id),
              ECUIO_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    ASSERT_TRUE(wait_for_identity(
        session, id,
        [](const ecuio_channel_identity_t& r) {
            return (r.flags & ECUIO_IDENTITY_COMPLETE) != 0;
        },
        5000))
        << "identification didn't complete";
    EXPECT_EQ(id.rx_id, 0x7E8u);
    EXPECT_EQ(id.items, ECUIO_IDENT_COUNT);
    EXPECT_EQ(id.generation, ECUIO_IDENT_COUNT);

    /* The rotation's first request, then the first item, and so on. */
    expect_write(0, 0x01, 0x04);
    expect_write(1, 0x09, 0x02);
    expect_write(2, 0x01, 0x0C);
    expect_write(3, 0x09, 0x04);

    const char* want_text[] = {
        "FAKEJ2534VIN00001", "FAKECAL-01", "DE AD BE EF", "FAKE-ECM",
        "FAKE-PART-0001", "FAKE-SW-0001", "FAKE 1.0", "FAKE SUPPLIER",
        "FAKE-SERIAL-000001", "FAKEJ2534VIN00001", "FAKE-HW-01",
        nullptr, nullptr};
    for (uint32_t i = 0; i < ECUIO_IDENT_COUNT; ++i) {
        const ecuio_identity_item_t& it = id.list[i];
        EXPECT_EQ(it.length, ECUIO_IDENT_REQUESTS[i].len) << "item " << i;
        EXPECT_EQ(memcmp(it.request, ECUIO_IDENT_REQUESTS[i].bytes, it.length), 0)
            << "item " << i;
        if (want_text[i] != nullptr) {
            EXPECT_EQ(it.outcome, ECUIO_IDENTITY_ANSWERED) << "item " << i;
            EXPECT_STREQ(it.text, want_text[i]) << "item " << i;
            EXPECT_EQ(it.text_length, strlen(want_text[i])) << "item " << i;
        } else {
            EXPECT_EQ(it.outcome, ECUIO_IDENTITY_REFUSED) << "item " << i;
            EXPECT_EQ(it.nrc, 0x31u) << "item " << i;
            EXPECT_EQ(it.text[0], '\0') << "item " << i;
        }
    }

    /* Asked once: among the writes the fake recorded, 09 02 appears once,
     * and the refusals dropped nothing from the rotation. */
    unsigned long wc = 0;
    get_write_count(&wc);
    unsigned vin_asks = 0;
    for (unsigned long i = 0; i < wc && i < 16; ++i) {
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(i, wbuf, &wlen, &wflags);
        vin_asks += (wlen == 6 && wbuf[4] == 0x09 && wbuf[5] == 0x02) ? 1u : 0u;
    }
    EXPECT_EQ(vin_asks, 1u);
    ecuio_channel_support_t support{};
    ASSERT_EQ(ecuio_session_get_channel_support(session, 0, &support), ECUIO_OK);
    EXPECT_EQ(support.dropped, 0u);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* ?identify=0&diagnose=0 asks nothing of the sort: the first writes are all the
 * rotation's (diagnostics off too), and the report has no items. ?poll=
 * has none either. */
TEST_F(J2534AdoptionTest, IdentifyOffSendsNoIdentificationRequest)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=obd2&probe=0&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ecuio_channel_identity_t id{};
    ASSERT_EQ(ecuio_session_get_channel_identity(session, 0, &id), ECUIO_OK);
    EXPECT_EQ(id.items, 0u);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    wait_for_writes(get_write_count, 6, 300);
    for (unsigned long i = 0; i < 6; ++i) {
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_EQ(wlen, 6u) << "write " << i;
        EXPECT_EQ(wbuf[4], 0x01u) << "write " << i;
    }
    ASSERT_EQ(ecuio_session_get_channel_identity(session, 0, &id), ECUIO_OK);
    EXPECT_EQ(id.items, 0u);
    EXPECT_EQ(id.flags, 0u);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);

    snprintf(uri, sizeof(uri), "j2534://%s?profile=obd2&poll=0x0C",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_get_channel_identity(session, 0, &id), ECUIO_OK);
    EXPECT_EQ(id.items, 0u) << "?poll= asks its one PID alone";
    ecuio_session_destroy(session);
}

/* Diagnostics (ADR-0037) take their turns with the rotation, the code
 * lists as one-byte requests, and fill the report with the fake ECU's
 * answers: its codes (the stored list a First Frame on ISO-TP), every
 * monitor and the freeze frame's own values. Identification off, so
 * the list starts at the second write. */
TEST_F(J2534AdoptionTest, DiagnosticsReadTheFakeEcusReport)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=obd2&probe=0&identify=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(session, 0, &d), ECUIO_OK);
    EXPECT_EQ(d.rx_id, 0x7E8u);
    EXPECT_EQ(d.flags, ECUIO_DIAG_ASKED) << "asked, nothing answered yet";
    EXPECT_EQ(d.generation, 0u);
    EXPECT_EQ(ecuio_session_get_channel_diagnostics(session, 1, &d),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_channel_diagnostics(session, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_channel_diagnostics(nullptr, 0, &d),
              ECUIO_ERR_NULL_ARGUMENT);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);

    ASSERT_TRUE(wait_for_diagnostics(
        session, d,
        [](const ecuio_channel_diagnostics_t& r) {
            return (r.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        5000))
        << "diagnostics didn't complete";
    EXPECT_EQ(d.rx_id, 0x7E8u);
    EXPECT_EQ(d.generation, fake_diagnostics::kItems) << "one per item settled";
    fake_diagnostics::expect_report(d);

    /* The rotation's first request, then readiness, then the rotation
     * and this drive cycle's (PID 41), then each code list, a service
     * byte alone, the rotation between each */
    expect_write(0, 0x01, 0x04);
    expect_write(1, 0x01, 0x01);
    expect_write(2, 0x01, 0x0C);
    expect_write(3, 0x01, 0x41);
    const uint8_t lists[] = {0x03, 0x07, 0x0A};
    for (unsigned long k = 0; k < 3; ++k) {
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(5 + 2 * k, wbuf, &wlen, &wflags);
        EXPECT_EQ(wlen, 5u) << "write " << 5 + 2 * k;
        EXPECT_EQ(wbuf[4], lists[k]) << "write " << 5 + 2 * k;
    }
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);
}

/* ?diagnose=0 asks nothing of the sort, and its report says so: no
 * ECUIO_DIAG_ASKED, nothing settled, even after a Read again. ?poll=
 * reads none either. */
TEST_F(J2534AdoptionTest, DiagnoseOffAsksNoDiagnostics)
{
    char uri[700];
    snprintf(uri, sizeof(uri),
             "j2534://%s?profile=obd2&probe=0&identify=0&diagnose=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_diagnostics(session), ECUIO_OK);
    wait_for_writes(get_write_count, 8, 300);
    for (unsigned long i = 0; i < 8; ++i) {
        unsigned char wbuf[64] = {};
        unsigned long wlen = 0, wflags = 0;
        get_recorded_write(i, wbuf, &wlen, &wflags);
        ASSERT_EQ(wlen, 6u) << "write " << i;
        EXPECT_EQ(wbuf[4], 0x01u) << "write " << i;
        EXPECT_NE(wbuf[5], 0x01u) << "write " << i << ": no readiness";
    }
    ecuio_channel_diagnostics_t d{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(session, 0, &d), ECUIO_OK);
    EXPECT_EQ(d.rx_id, 0x7E8u);
    EXPECT_EQ(d.flags, 0u);
    EXPECT_EQ(d.generation, 0u);
    EXPECT_EQ(d.readiness, ECUIO_DIAG_UNASKED);
    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    ecuio_session_destroy(session);

    snprintf(uri, sizeof(uri), "j2534://%s?profile=obd2&poll=0x0C",
             FAKE_J2534_DLL_PATH);
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(session, 0, &d), ECUIO_OK);
    EXPECT_EQ(d.flags, 0u) << "?poll= asks its one PID alone";
    ecuio_session_destroy(session);
}

/* Read again (ecuio_session_read_diagnostics) asks the whole list once
 * more after the first read settles: the report starts over, moves its
 * generation on, and fills again. After close the call is refused and
 * the report keeps its last update. */
TEST_F(J2534AdoptionTest, ReadAgainAsksTheListOnceMore)
{
    char uri[700];
    snprintf(uri, sizeof(uri), "j2534://%s?profile=obd2&probe=0&identify=0",
             FAKE_J2534_DLL_PATH);
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_diagnostics(nullptr), ECUIO_ERR_NULL_ARGUMENT);
    ASSERT_EQ(ecuio_session_start(session), ECUIO_OK);
    ecuio_channel_diagnostics_t d{};
    const auto complete = [](const ecuio_channel_diagnostics_t& r) {
        return (r.flags & ECUIO_DIAG_COMPLETE) != 0;
    };
    ASSERT_TRUE(wait_for_diagnostics(session, d, complete, 5000))
        << "the first read didn't complete";
    ASSERT_EQ(d.generation, fake_diagnostics::kItems);

    ASSERT_EQ(ecuio_session_read_diagnostics(session), ECUIO_OK);
    const uint32_t again =
        fake_diagnostics::kItems + 1 + fake_diagnostics::kItems;
    ASSERT_TRUE(wait_for_diagnostics(
        session, d,
        [again](const ecuio_channel_diagnostics_t& r) {
            return r.generation == again &&
                   (r.flags & ECUIO_DIAG_COMPLETE) != 0;
        },
        5000))
        << "the second read didn't complete; generation " << d.generation;
    fake_diagnostics::expect_report(d);

    EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_diagnostics(session),
              ECUIO_ERR_SESSION_CLOSED);
    ecuio_channel_diagnostics_t after{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(session, 0, &after),
              ECUIO_OK);
    EXPECT_EQ(after.generation, again) << "the last update, kept";
    EXPECT_EQ(memcmp(&after, &d, sizeof d), 0);
    ecuio_session_destroy(session);
}

} // namespace

#endif // _WIN32
