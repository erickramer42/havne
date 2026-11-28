// File/layer: tests/test_core_vin_focus.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: CoreTest (test_core)

#include "core_test_support.h"

using namespace core_test;

/* The focus call clears the thread's detail on entry, so a refusal that
 * sets none (a NULL, a closed session) or a success leaves no stale
 * words from an earlier call */
TEST(CoreTest, SetFocusClearsAStaleDetail)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    ASSERT_GE(set->count, 1u);
    const uint16_t id = set->defs[0].id;
    ecuio_detail_set("stale");
    EXPECT_EQ(ecuio_session_set_focus(nullptr, &id, 1),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    ecuio_detail_set("stale");
    EXPECT_EQ(ecuio_session_set_focus(s, &id, 1), ECUIO_OK);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    ecuio_detail_set("stale");
    EXPECT_EQ(ecuio_session_set_focus(s, &id, 1), ECUIO_ERR_SESSION_CLOSED);
    EXPECT_STREQ(ecuio_last_error_detail(), "");
    ecuio_session_destroy(s);
}

namespace {

/* The id of set's first def whose request is bytes[0..len) */
uint16_t focus_id_of(const ecuio_signal_set_t* set,
                     std::initializer_list<uint8_t> bytes)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        uint8_t req[3] = {};
        uint8_t len = 0;
        if (ecuio_engine_request_for_def(&set->defs[i], req, &len) &&
            len == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), req)) {
            return set->defs[i].id;
        }
    }
    ADD_FAILURE() << "no def asks that request";
    return 0;
}

} // namespace

/* A requesting session refuses a focus that leaves it nothing to ask,
 * with the reason in the detail and the focus it had untouched; a focus
 * with one askable pick among others is taken. Under ?poll=0x0C the one
 * request is 01 0C, so a focus on 0x0D alone is refused. The requests
 * are built on a mock session, whose worker never takes the focus. */
TEST(CoreTest, AFocusThatLeavesNothingToRequestIsRefused)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus?profile=obd2", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    const uint16_t rpm = focus_id_of(set, {0x01, 0x0C});
    const uint16_t speed = focus_id_of(set, {0x01, 0x0D});
    ASSERT_NE(rpm, speed);
    request_params params;
    params.has_poll = true;
    params.poll_pid = 0x0C;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   e.get(), &probe, &identify),
              ECUIO_OK);
    ASSERT_EQ(e->count, 1u);

    ASSERT_EQ(ecuio_session_set_focus(s, &rpm, 1), ECUIO_OK);
    const uint32_t generation = s->focus_generation.load();
    EXPECT_EQ(ecuio_session_set_focus(s, &speed, 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the focus leaves nothing to request: none of the picked "
                 "signals is asked on this session");
    EXPECT_EQ(s->focus_generation.load(), generation) << "nothing changed";
    {
        std::lock_guard<std::mutex> lock(s->focus_mutex);
        EXPECT_EQ(s->focus_ids, std::vector<uint16_t>{rpm});
    }
    const uint16_t both[2] = {speed, rpm};
    EXPECT_EQ(ecuio_session_set_focus(s, both, 2), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(s, nullptr, 0), ECUIO_OK);
    ecuio_session_destroy(s);
}

/* ADR-0051's appendix of 2026-11-10: a computed pick keeps its inputs'
 * requests in rotation, never among the caller's picks. obd2's Boost alone is
 * taken and keeps MAP (01 0B) and baro (01 33) asked, parking the rest; under
 * ?poll=0x0C, which asks neither, it leaves nothing to request. */
TEST(CoreTest, AComputedPickKeepsItsInputsRequested)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus?profile=obd2", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    const uint16_t boost = ECUIO_OBD2_SIG_BOOST_ID;
    const uint16_t rpm = focus_id_of(set, {0x01, 0x0C});
    request_params params;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   e.get(), &probe, &identify),
              ECUIO_OK);
    ecuio_focus_seen seen;
    ecuio_take_focus(s, e.get(), seen);

    ASSERT_EQ(ecuio_session_set_focus(s, &boost, 1), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_take_focus(s, e.get(), seen);
    int kept = 0;
    for (uint32_t k = 0; k < e->count; ++k) {
        const ecuio_request& r = e->requests[k];
        const bool input = r.len == 2 && r.bytes[0] == 0x01 &&
                           (r.bytes[1] == 0x0B || r.bytes[1] == 0x33);
        EXPECT_EQ(r.parked == 0, input)
            << "PID " << std::hex << int(r.bytes[1]);
        kept += r.parked == 0 ? 1 : 0;
    }
    EXPECT_EQ(kept, 2) << "MAP and baro";
    {
        std::lock_guard<std::mutex> lock(s->focus_mutex);
        EXPECT_EQ(s->focus_ids, std::vector<uint16_t>{boost})
            << "the caller's picks, never the inputs";
    }
    /* A pick beside it adds its own request */
    const uint16_t both[2] = {boost, rpm};
    ASSERT_EQ(ecuio_session_set_focus(s, both, 2), ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    kept = 0;
    for (uint32_t k = 0; k < e->count; ++k) {
        kept += e->requests[k].parked == 0 ? 1 : 0;
    }
    EXPECT_EQ(kept, 3);
    EXPECT_EQ(ecuio_session_set_focus(s, nullptr, 0), ECUIO_OK);
    ecuio_session_destroy(s);

    /* Inputs nothing asks leave nothing to request */
    ASSERT_EQ(ecuio_session_create("mock://focus?profile=obd2", &s), ECUIO_OK);
    set = ecuio_session_signal_set(s);
    request_params poll;
    poll.has_poll = true;
    poll.poll_pid = 0x0C;
    auto p = std::make_unique<ecuio_request_engine>();
    ASSERT_EQ(ecuio_build_requests(s, set, poll, s->info.rx_ids, s->info.tx_ids,
                                   s->info.channel_count, p.get(), &probe,
                                   &identify),
              ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(s, &boost, 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the focus leaves nothing to request: none of the picked "
                 "signals is asked on this session");
    ecuio_session_destroy(s);
}

/* ADR-0063 section 7: a focus keeps the requests of an attached
 * condition's signals asked, a computed signal's inputs for it, while the
 * focus's own picks stay the caller's. A disabled condition keeps
 * nothing; taking the conditions away parks what only they kept; with no
 * focus nothing is parked. The picks alone decide whether a focus is
 * askable: under ?poll=0x0C a condition on engine speed doesn't make a
 * focus on vehicle speed one. */
TEST(CoreTest, AFocusKeepsAnAttachedConditionsSignalsAsked)
{
    const auto attach = [](ecuio_session_t* s, const std::string& list) {
        const std::string json =
            R"({"havne_conditions":1,"conditions":[)" + list + "]}";
        ecuio_condition_set_t* set = nullptr;
        EXPECT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        const ecuio_status_t st = ecuio_session_set_conditions(s, set, nullptr);
        ecuio_condition_set_free(set);
        return st;
    };
    const auto on = [](const char* key, const char* signal, const char* unit,
                       const char* rest) {
        return std::string(R"({"key":")") + key + R"(","name":")" + key +
               R"(","when":{"signal":[")" + signal + R"("],"unit":")" + unit +
               R"(","above":1})" + rest + "}";
    };
    /* The PIDs kept in rotation, in PID order */
    const auto kept = [](const ecuio_request_engine& e) {
        std::vector<int> pids;
        for (uint32_t k = 0; k < e.count; ++k) {
            const ecuio_request& r = e.requests[k];
            if (r.parked == 0 && r.len == 2 && r.bytes[0] == 0x01) {
                pids.push_back(r.bytes[1]);
            }
        }
        std::sort(pids.begin(), pids.end());
        return pids;
    };

    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus?profile=obd2", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    const uint16_t rpm = focus_id_of(set, {0x01, 0x0C});
    request_params params;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   e.get(), &probe, &identify),
              ECUIO_OK);
    ecuio_focus_seen seen;
    ecuio_take_focus(s, e.get(), seen);

    /* No focus: a condition parks nothing and moves no focus generation */
    const uint32_t before = s->focus_generation.load();
    ASSERT_EQ(attach(s, on("t", "obd2.throttle", "%", "")), ECUIO_OK);
    EXPECT_EQ(s->focus_generation.load(), before);
    ecuio_take_focus(s, e.get(), seen);
    for (uint32_t k = 0; k < e->count; ++k) {
        EXPECT_EQ(e->requests[k].parked, 0) << "row " << k;
    }

    ASSERT_EQ(ecuio_session_set_focus(s, &rpm, 1), ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_EQ(kept(*e), (std::vector<int>{0x0C, 0x11})) << "throttle kept";
    {
        std::lock_guard<std::mutex> lock(s->focus_mutex);
        EXPECT_EQ(s->focus_ids, std::vector<uint16_t>{rpm});
    }

    /* The same conditions again move no generation, so no sweep restarts */
    const uint32_t focused = s->focus_generation.load();
    ASSERT_EQ(attach(s, on("t", "obd2.throttle", "%", "")), ECUIO_OK);
    EXPECT_EQ(s->focus_generation.load(), focused);

    /* Boost carries no request: its condition keeps MAP and baro, its
     * inputs; a disabled condition keeps nothing */
    ASSERT_EQ(
        attach(s, on("b", "id:7011", "kPa", "") + "," +
                      on("off", "obd2.speed", "km/h", R"(,"enabled":false)")),
        ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_EQ(kept(*e), (std::vector<int>{0x0B, 0x0C, 0x33}));

    /* Taking the conditions away parks what only they kept */
    ASSERT_EQ(ecuio_session_set_conditions(s, nullptr, nullptr), ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_EQ(kept(*e), std::vector<int>{0x0C});
    EXPECT_EQ(ecuio_session_set_focus(s, nullptr, 0), ECUIO_OK);
    ecuio_session_destroy(s);

    /* The picks alone decide: one request, 01 0C, under ?poll= */
    ASSERT_EQ(ecuio_session_create("mock://focus?profile=obd2", &s), ECUIO_OK);
    set = ecuio_session_signal_set(s);
    const uint16_t speed = focus_id_of(set, {0x01, 0x0D});
    request_params poll;
    poll.has_poll = true;
    poll.poll_pid = 0x0C;
    auto p = std::make_unique<ecuio_request_engine>();
    ASSERT_EQ(ecuio_build_requests(s, set, poll, s->info.rx_ids, s->info.tx_ids,
                                   s->info.channel_count, p.get(), &probe,
                                   &identify),
              ECUIO_OK);
    ASSERT_EQ(attach(s, on("r", "obd2.rpm", "RPM", "")), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus(s, &speed, 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_session_destroy(s);
}

/* ecuio_session_get_unasked_signals: the defs a focus parks, a computed
 * one with a parked input, in the set's order; a def that shares a pick's
 * reply, a dropped request's and a passive def are never listed; no
 * focus lists nothing, and the list stays readable after close. The
 * requests are built on mock sessions and the worker's take is driven by
 * hand, as in the cases above. */
TEST(CoreTest, TheUnaskedSignalsAreWhatTheFocusParks)
{
    const auto unasked = [](const ecuio_session_t* s) {
        uint32_t n = 0;
        EXPECT_EQ(ecuio_session_get_unasked_signals(s, nullptr, 0, &n),
                  ECUIO_OK);
        std::vector<uint16_t> ids(n);
        uint32_t again = 0;
        EXPECT_EQ(ecuio_session_get_unasked_signals(s, ids.data(), n, &again),
                  ECUIO_OK);
        EXPECT_EQ(again, n);
        return ids;
    };
    const auto pid_of = [](const ecuio_signal_def_t& d) {
        return d.mux_length == 2 && d.mux_offset == 0 &&
                       (d.mux_key >> 8) == 0x41
                   ? static_cast<int>(d.mux_key & 0xFF)
                   : -1;
    };
    const auto ids_of = [](const ecuio_signal_set_t* set, auto keep) {
        std::vector<uint16_t> ids;
        for (uint32_t i = 0; i < set->count; ++i) {
            if (keep(set->defs[i])) {
                ids.push_back(set->defs[i].id);
            }
        }
        return ids;
    };

    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://unasked?profile=obd2", &s),
              ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    request_params params;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   e.get(), &probe, &identify),
              ECUIO_OK);
    ecuio_focus_seen seen;
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_TRUE(unasked(s).empty()) << "no focus";

    /* A focus on O2 S1's first field (PID 24): its second shares the reply */
    uint16_t lambda = 0;
    for (uint32_t i = 0; i < set->count && lambda == 0; ++i) {
        if (pid_of(set->defs[i]) == 0x24) {
            lambda = set->defs[i].id;
        }
    }
    ASSERT_NE(lambda, 0);
    ASSERT_EQ(ecuio_session_set_focus(s, &lambda, 1), ECUIO_OK);
    EXPECT_TRUE(unasked(s).empty()) << "the worker hasn't taken it yet";
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_EQ(unasked(s),
              ids_of(set,
                     [&](const ecuio_signal_def_t& d) {
                         const bool computed =
                             (d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0;
                         return computed ||
                                (pid_of(d) >= 0 && pid_of(d) != 0x24);
                     }))
        << "every other Service 01 def, and Boost, whose MAP is parked";

    /* A focus on Boost asks MAP and baro, so Boost isn't listed */
    const uint16_t boost = ECUIO_OBD2_SIG_BOOST_ID;
    ASSERT_EQ(ecuio_session_set_focus(s, &boost, 1), ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_EQ(unasked(s), ids_of(set, [&](const ecuio_signal_def_t& d) {
                  const int pid = pid_of(d);
                  return pid >= 0 && pid != 0x0B && pid != 0x33;
              }));

    /* A dropped request leaves the list as the table changes */
    uint32_t rpm_row = e->count;
    for (uint32_t k = 0; k < e->count; ++k) {
        if (e->requests[k].len == 2 && e->requests[k].bytes[1] == 0x0C) {
            rpm_row = k;
        }
    }
    ASSERT_LT(rpm_row, e->count);
    ecuio_engine_drop(e.get(), rpm_row, ECUIO_DROP_NOT_SUPPORTED, 0x12);
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_EQ(unasked(s), ids_of(set, [&](const ecuio_signal_def_t& d) {
                  const int pid = pid_of(d);
                  return pid >= 0 && pid != 0x0B && pid != 0x33 && pid != 0x0C;
              }));

    /* The capacity takes the first, the count is all of them */
    const std::vector<uint16_t> all = unasked(s);
    ASSERT_GE(all.size(), 2u);
    uint16_t two[2] = {};
    uint32_t n = 0;
    EXPECT_EQ(ecuio_session_get_unasked_signals(s, two, 2, &n), ECUIO_OK);
    EXPECT_EQ(n, all.size());
    EXPECT_EQ(two[0], all[0]);
    EXPECT_EQ(two[1], all[1]);
    EXPECT_EQ(ecuio_session_get_unasked_signals(nullptr, two, 2, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_unasked_signals(s, nullptr, 2, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_unasked_signals(s, two, 2, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);

    /* Readable after close */
    ecuio_session_close(s);
    EXPECT_EQ(unasked(s), all);
    ecuio_session_destroy(s);

    /* The focus's end lists nothing */
    ASSERT_EQ(ecuio_session_create("mock://unasked?profile=obd2", &s),
              ECUIO_OK);
    set = ecuio_session_signal_set(s);
    auto f = std::make_unique<ecuio_request_engine>();
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   f.get(), &probe, &identify),
              ECUIO_OK);
    ecuio_focus_seen fresh;
    ASSERT_EQ(ecuio_session_set_focus(s, &boost, 1), ECUIO_OK);
    ecuio_take_focus(s, f.get(), fresh);
    EXPECT_FALSE(unasked(s).empty());
    ASSERT_EQ(ecuio_session_set_focus(s, nullptr, 0), ECUIO_OK);
    ecuio_take_focus(s, f.get(), fresh);
    EXPECT_TRUE(unasked(s).empty()) << "the focus ended";
    ecuio_session_destroy(s);

    /* A passive def has no request, so it is never unasked */
    static ecuio_signal_def_t defs[3];
    defs[0] = value_def(61001, ECUIO_VALUE_UINT, ECUIO_OBD2_ECU_ARB_ID);
    defs[1] = value_def(61002, ECUIO_VALUE_UINT, 0x3E8);
    defs[2] = value_def(61003, ECUIO_VALUE_UINT, ECUIO_OBD2_ECU_ARB_ID);
    for (ecuio_signal_def_t& d : defs) {
        d.bit_length = 8;
    }
    for (uint32_t i : {0u, 2u}) {
        defs[i].mux_length = 2;
        defs[i].start_byte = 2;
    }
    defs[0].mux_key = 0x410C;
    defs[2].mux_key = 0x410D;
    static ecuio_channel_decl_t channel[1] = {
        {ECUIO_OBD2_ECU_ARB_ID, 0x7E0, 0, 0, {}}};
    static ecuio_signal_set_t passive{};
    passive.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    passive.defs = defs;
    passive.count = 3;
    passive.channels = channel;
    passive.channel_count = 1;
    ASSERT_EQ(ecuio_profile_register("unasked-passive", &passive, nullptr),
              ECUIO_OK);
    ASSERT_EQ(
        ecuio_session_create("mock://unasked?profile=unasked-passive", &s),
        ECUIO_OK);
    auto g = std::make_unique<ecuio_request_engine>();
    ASSERT_EQ(ecuio_build_requests(s, &passive, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   g.get(), &probe, &identify),
              ECUIO_OK);
    ecuio_focus_seen third;
    const uint16_t first = 61001;
    ASSERT_EQ(ecuio_session_set_focus(s, &first, 1), ECUIO_OK);
    ecuio_take_focus(s, g.get(), third);
    EXPECT_EQ(unasked(s), std::vector<uint16_t>{61003});
    ecuio_session_destroy(s);
    EXPECT_EQ(ecuio_profile_unregister("unasked-passive"), ECUIO_OK);
}

/* ADR-0062 section 3: a computed signal is absent, unasked or not
 * received only with an input its program can't give a value without.
 * With MAF absent, parked by a focus on speed alone, or its request
 * dropped (a condition on each signal), a MAF read only in a branch an if may
 * not take, or as one part of an any or an all, lists nothing; MAF in
 * the predicate, in both branches or in a sum lists the signal, and a
 * signal reading either follows it */
TEST(CoreTest, AComputedSignalReadingAnInputOnlyInABranchIsNotListedWithIt)
{
    const std::string file = R"json({"havne_signals": 2, "name": "Branches",
 "source": {"name": "Havne's tests"}, "extends": "obd2", "signals": [
 {"key": "per_speed", "name": "Per Speed", "kind": "computed", "expr": "if(obd2.speed > 0, obd2.maf / obd2.speed, 0)"},
 {"key": "gate", "name": "Gate", "kind": "computed", "expr": "if(obd2.maf > 0, obd2.speed, 0)"},
 {"key": "either", "name": "Either", "kind": "computed", "expr": "if(obd2.speed > 0, obd2.maf, obd2.maf * 2)"},
 {"key": "moving", "name": "Moving", "kind": "computed", "expr": "if(any(obd2.speed > 0, obd2.maf > 0), 1, 0)"},
 {"key": "both", "name": "Both", "kind": "computed", "expr": "if(all(obd2.speed > 0, obd2.maf > 0), 1, 0)"},
 {"key": "sum", "name": "Sum", "kind": "computed", "expr": "obd2.maf + obd2.speed"},
 {"key": "per_speed_2", "name": "Per Speed 2", "kind": "computed", "expr": "per_speed * 2"},
 {"key": "gate_2", "name": "Gate 2", "kind": "computed", "expr": "gate * 2"}]})json";
    ecuio_signal_file_t* sf = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(file.data(), file.size(), &sf), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_profile_register(ecuio_signal_file_profile(sf),
                                     ecuio_signal_file_set(sf), nullptr),
              ECUIO_OK);
    const auto id_of = [&](const char* key) {
        const ecuio_signal_def_t* d = ecuio_signal_file_find(sf, key);
        EXPECT_NE(d, nullptr) << key;
        return d != nullptr ? d->id : uint16_t{0};
    };
    /* The file's signals each list holds, in the file's order */
    const auto listed = [&](const std::vector<uint16_t>& ids) {
        std::vector<std::string> keys;
        for (const char* key : {"per_speed", "gate", "either", "moving", "both",
                                "sum", "per_speed_2", "gate_2"}) {
            if (std::find(ids.begin(), ids.end(), id_of(key)) != ids.end()) {
                keys.push_back(key);
            }
        }
        return keys;
    };
    const std::vector<std::string> want = {"gate", "either", "sum", "gate_2"};
    const std::string uri =
        std::string("mock://branches?profile=") + ecuio_signal_file_profile(sf);

    /* Absent: three replies to 01 10 with no data byte, decoded for a
     * values callback */
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK);
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(s, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t none = value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x10});
    for (uint32_t k = 0; k < ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(s, &none);
    }
    uint32_t n = 0;
    ASSERT_EQ(ecuio_session_get_absent_signals(s, nullptr, 0, &n), ECUIO_OK);
    std::vector<uint16_t> absent(n);
    ASSERT_EQ(ecuio_session_get_absent_signals(s, absent.data(), n, &n),
              ECUIO_OK);
    ASSERT_FALSE(absent.empty());
    EXPECT_EQ(absent[0], ECUIO_OBD2_SIG_MAF_ID);
    EXPECT_EQ(listed(absent), want);
    EXPECT_EQ(absent.size(), 1 + want.size()) << "MAF and its four readers";
    ecuio_session_destroy(s);

    /* Unasked: a focus on speed parks MAF */
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    request_params params;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   e.get(), &probe, &identify),
              ECUIO_OK);
    ecuio_focus_seen seen;
    const uint16_t speed = ECUIO_OBD2_SIG_SPEED_ID;
    ASSERT_EQ(ecuio_session_set_focus(s, &speed, 1), ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    n = 0;
    ASSERT_EQ(ecuio_session_get_unasked_signals(s, nullptr, 0, &n), ECUIO_OK);
    std::vector<uint16_t> unasked(n);
    ASSERT_EQ(ecuio_session_get_unasked_signals(s, unasked.data(), n, &n),
              ECUIO_OK);
    EXPECT_NE(std::find(unasked.begin(), unasked.end(), ECUIO_OBD2_SIG_MAF_ID),
              unasked.end());
    EXPECT_EQ(std::find(unasked.begin(), unasked.end(), speed), unasked.end());
    EXPECT_EQ(listed(unasked), want);
    ecuio_session_destroy(s);

    /* Not received: MAF's request dropped, a condition on each signal */
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &s), ECUIO_OK);
    set = ecuio_session_signal_set(s);
    auto d = std::make_unique<ecuio_request_engine>();
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   d.get(), &probe, &identify),
              ECUIO_OK);
    uint32_t maf_row = d->count;
    for (uint32_t k = 0; k < d->count; ++k) {
        if (d->requests[k].len == 2 && d->requests[k].bytes[1] == 0x10) {
            maf_row = k;
        }
    }
    ASSERT_LT(maf_row, d->count);
    ecuio_engine_drop(d.get(), maf_row, ECUIO_DROP_NOT_SUPPORTED, 0x12);
    ecuio_focus_seen dropped;
    ecuio_take_focus(s, d.get(), dropped);
    const char* keys[] = {"per_speed", "gate", "either",      "moving",
                          "both",      "sum",  "per_speed_2", "gate_2"};
    std::string list;
    for (const char* key : keys) {
        list += std::string(list.empty() ? "" : ",") + R"({"key":")" + key +
                R"(","name":")" + key + R"(","when":{"signal":["key:)" + key +
                R"("],"unit":"","above":1}})";
    }
    const std::string json =
        R"({"havne_conditions":1,"conditions":[)" + list + "]}";
    ecuio_condition_set_t* conditions = nullptr;
    ASSERT_EQ(ecuio_condition_set_load(json.data(), json.size(), &conditions),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_set_conditions(s, conditions, sf), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_condition_set_free(conditions);
    s->ran = true; /* as a worker that has run: the states judge the lists */
    ecuio_condition_state_t states[8] = {};
    n = 0;
    ASSERT_EQ(ecuio_session_get_condition_states(s, states, 8, &n), ECUIO_OK);
    ASSERT_EQ(n, 8u);
    std::vector<std::string> not_received;
    for (uint32_t i = 0; i < n; ++i) {
        if (states[i].why == ECUIO_CONDITION_WHY_NOT_RECEIVED) {
            not_received.push_back(keys[i]);
        }
    }
    EXPECT_EQ(not_received, want);

    /* Speed absent too: an any or all over a dropped MAF and an absent
     * speed lacks a value, the two counted together */
    ValuesCapture fed;
    ASSERT_EQ(ecuio_register_values_callback(s, values_capture_cb, &fed),
              ECUIO_OK);
    const ecuio_frame_t no_speed =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0D});
    for (uint32_t k = 0; k < ECUIO_ABSENT_AFTER_FRAMES; ++k) {
        ecuio_session_dispatch_callbacks(s, &no_speed);
    }
    ASSERT_EQ(ecuio_session_get_condition_states(s, states, 8, &n), ECUIO_OK);
    not_received.clear();
    for (uint32_t i = 0; i < n; ++i) {
        if (states[i].why == ECUIO_CONDITION_WHY_NOT_RECEIVED) {
            not_received.push_back(keys[i]);
        }
    }
    EXPECT_EQ(not_received,
              std::vector<std::string>(std::begin(keys), std::end(keys)));
    ecuio_session_destroy(s);

    EXPECT_EQ(ecuio_profile_unregister(ecuio_signal_file_profile(sf)),
              ECUIO_OK);
    ecuio_signal_file_free(sf);
}

/* ADR-0056 section 4: the call's refusals, each changing nothing, and
 * the minimum reaching the engine with the focus at the worker's take;
 * the focus's end takes it away. A default pick (ECUIO_FOCUS_DEFAULT_PICK)
 * is taken as any focus and needs picks; another flag is refused. */
TEST(CoreTest, SetFocusWithChecksItsMinimumAndTheWorkerTakesIt)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus?profile=obd2", &s), ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    const uint16_t rpm = focus_id_of(set, {0x01, 0x0C});
    request_params params;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   e.get(), &probe, &identify),
              ECUIO_OK);
    ecuio_focus_seen seen;
    ecuio_take_focus(s, e.get(), seen);

    EXPECT_EQ(ecuio_session_set_focus_with(nullptr, &rpm, 1, 100, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_set_focus_with(s, nullptr, 1, 100, 0),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_EQ(info.flags & ECUIO_SESSION_INFO_REQUESTS, 0u)
        << "the mock asks nothing";
    const uint32_t generation = s->focus_generation.load();
    EXPECT_EQ(ecuio_session_set_focus_with(s, &rpm, 1, 100,
                                           ECUIO_FOCUS_DEFAULT_PICK | 0x2u),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(), "the focus takes no flag 0x2");
    EXPECT_EQ(ecuio_session_set_focus_with(s, nullptr, 0, 0,
                                           ECUIO_FOCUS_DEFAULT_PICK),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "a default pick goes with signals picked: with none, set no "
                 "focus and the whole set is asked");
    EXPECT_EQ(ecuio_session_set_focus_with(s, &rpm, 1,
                                           ECUIO_FOCUS_MIN_SWEEP_MAX_MS + 1, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "a minimum time between sweeps is at most 60000 ms");
    EXPECT_EQ(ecuio_session_set_focus_with(s, nullptr, 0, 100, 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(s->focus_generation.load(), generation) << "nothing changed";

    ASSERT_EQ(ecuio_session_set_focus_with(s, &rpm, 1,
                                           ECUIO_FOCUS_MIN_SWEEP_MAX_MS, 0),
              ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_TRUE(e->focused);
    EXPECT_EQ(e->min_sweep_us, 60000 * int64_t{1000});

    ASSERT_EQ(ecuio_session_set_focus_with(s, &rpm, 1, 250, 0), ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_EQ(e->min_sweep_us, 250000);

    /* A default pick is taken as any focus, its flag the session's */
    const auto in_rotation = [&] {
        uint32_t n = 0;
        for (uint32_t k = 0; k < e->count; ++k) {
            n += e->requests[k].parked == 0 ? 1 : 0;
        }
        return n;
    };
    ASSERT_EQ(
        ecuio_session_set_focus_with(s, &rpm, 1, 0, ECUIO_FOCUS_DEFAULT_PICK),
        ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_TRUE(e->focused);
    EXPECT_EQ(in_rotation(), 1u);
    EXPECT_EQ(s->focus_flags, ECUIO_FOCUS_DEFAULT_PICK);

    /* The plain call is the same focus with none, and no flag */
    ASSERT_EQ(ecuio_session_set_focus(s, &rpm, 1), ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_TRUE(e->focused);
    EXPECT_EQ(e->min_sweep_us, 0);
    EXPECT_EQ(in_rotation(), 1u);
    EXPECT_EQ(s->focus_flags, 0u);

    ASSERT_EQ(ecuio_session_set_focus_with(s, &rpm, 1, 250, 0), ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    ASSERT_EQ(ecuio_session_set_focus_with(s, nullptr, 0, 0, 0), ECUIO_OK);
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_FALSE(e->focused);
    EXPECT_EQ(e->min_sweep_us, 0);

    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_set_focus_with(s, &rpm, 1, 250, 0),
              ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);
}

/* uds-generic: a DID the scan finds under a focus joins parked and is
 * unparked at the worker's next take when the focus names it; one it
 * doesn't name stays parked. A focus on a DID the scan hasn't found is
 * refused alone, and taken once it has. Rows are added as the scan's
 * find adds them (ecuio_engine_scan_poll). */
TEST(CoreTest, AScannedDidFoundUnderAFocusJoinsItWhenPicked)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(
        ecuio_session_create(
            "mock://focus?profile=uds-generic&did=1E1C:u16,1E18-1E1A", &s),
        ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    const uint16_t polled = focus_id_of(set, {0x22, 0x1E, 0x1C});
    const uint16_t picked = focus_id_of(set, {0x22, 0x1E, 0x19});
    request_params params;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   e.get(), &probe, &identify),
              ECUIO_OK);
    ASSERT_EQ(e->count, 1u) << "the polled DID; the scan's are unasked";

    EXPECT_EQ(ecuio_session_set_focus(s, &picked, 1),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "the scan hasn't found it";
    const uint16_t ids[2] = {polled, picked};
    ASSERT_EQ(ecuio_session_set_focus(s, ids, 2), ECUIO_OK);
    ecuio_focus_seen seen;
    ecuio_take_focus(s, e.get(), seen);
    ASSERT_TRUE(e->focused);
    EXPECT_EQ(e->requests[0].parked, 0u);

    for (const uint8_t lo : {uint8_t{0x19}, uint8_t{0x1A}}) {
        const uint8_t req[3] = {0x22, 0x1E, lo};
        ASSERT_EQ(ecuio_engine_add(e.get(), 0, req, 3),
                  ecuio_engine_add_result::added);
        ++e->chans[0].generation; /* the scan's settle */
    }
    EXPECT_EQ(e->requests[1].parked, 1u) << "it joins parked";
    ecuio_take_focus(s, e.get(), seen);
    EXPECT_EQ(e->requests[1].parked, 0u) << "1E19 is picked";
    EXPECT_EQ(e->requests[2].parked, 1u) << "1E1A isn't";
    EXPECT_EQ(e->chans[0].requests, 2u);
    EXPECT_EQ(ecuio_session_set_focus(s, &picked, 1), ECUIO_OK)
        << "found, it is askable";
    ecuio_session_destroy(s);
}

/* A scanned DID found under a focus with a minimum time between sweeps
 * neither starts the sweep under way over nor ends its wait: DIDs found
 * faster than a sweep runs would otherwise keep it from ever ending */
TEST(CoreTest, AScannedDidJoiningLeavesTheSweepUnderWayAlone)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://focus?profile=uds-generic"
                                   "&did=1E1C:u16,1E1D:u16,1E18-1E1A",
                                   &s),
              ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_session_signal_set(s);
    ASSERT_NE(set, nullptr);
    request_params params;
    params.probe = false; /* the rotation alone */
    params.identify = false;
    params.diagnose = false;
    auto e = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    ASSERT_EQ(ecuio_build_requests(s, set, params, s->info.rx_ids,
                                   s->info.tx_ids, s->info.channel_count,
                                   e.get(), &probe, &identify),
              ECUIO_OK);
    ASSERT_EQ(e->count, 2u) << "the polled DIDs; the scan's are unasked";
    /* The finds come by hand (join): the scan's own requests stay out */
    e->chans[0].scan_pending = 0;
    e->chans[0].scanning = false;
    const uint16_t ids[3] = {focus_id_of(set, {0x22, 0x1E, 0x1C}),
                             focus_id_of(set, {0x22, 0x1E, 0x1D}),
                             focus_id_of(set, {0x22, 0x1E, 0x19})};
    ASSERT_EQ(ecuio_session_set_focus_with(s, ids, 3, 500, 0), ECUIO_OK);
    ecuio_focus_seen seen;
    ecuio_take_focus(s, e.get(), seen);
    ASSERT_EQ(e->min_sweep_us, 500000);

    const auto ask = [&e](int64_t at) {
        const int32_t r = ecuio_engine_due(e.get(), 0, at);
        if (r != ECUIO_ENGINE_NONE) {
            ecuio_engine_sent(e.get(), 0, r, true, at);
            (void)ecuio_engine_expire(e.get(), 0, at + ECUIO_ENGINE_WAIT_US);
        }
        return r;
    };
    const auto join = [&](uint8_t lo) {
        const uint8_t req[3] = {0x22, 0x1E, lo};
        EXPECT_EQ(ecuio_engine_add(e.get(), 0, req, 3),
                  ecuio_engine_add_result::added);
        ++e->chans[0].generation; /* the scan's settle */
        ecuio_take_focus(s, e.get(), seen);
    };
    EXPECT_NE(ask(0), ECUIO_ENGINE_NONE);
    join(0x19); /* picked: the rotation is three rows now */
    ASSERT_EQ(e->chans[0].requests, 3u);
    EXPECT_NE(ask(120000), ECUIO_ENGINE_NONE);
    EXPECT_NE(ask(240000), ECUIO_ENGINE_NONE);
    EXPECT_EQ(ask(360000), ECUIO_ENGINE_NONE)
        << "three rows asked: the sweep ended, counted from its start";
    EXPECT_EQ(e->chans[0].next_sweep_us, 500000);
    join(0x1A); /* not picked */
    EXPECT_EQ(ask(499999), ECUIO_ENGINE_NONE) << "the wait stands";
    EXPECT_NE(ask(500000), ECUIO_ENGINE_NONE);
    ecuio_session_destroy(s);
}
