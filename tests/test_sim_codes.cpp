#include "test_obd2_support.h"

#include <string>

namespace {

struct Sim {
    ecuio_sim_t* sim = nullptr;
    explicit Sim(const char* uri)
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri, 0, &sim), ECUIO_OK)
            << uri << ": " << ecuio_last_error_detail();
    }
    ~Sim() { ecuio_sim_destroy(sim); }
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;

    /* the whole reply to req on id, {} for none */
    Bytes ask(uint32_t id, const Bytes& req)
    {
        std::vector<ecuio_sim_reply_t> out(ECUIO_SIM_MAX_REPLIES);
        uint32_t n = 0;
        EXPECT_EQ(ecuio_sim_ask(sim, id, req.data(),
                                static_cast<uint32_t>(req.size()), t += 100000,
                                out.data(), static_cast<uint32_t>(out.size()),
                                &n),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        for (uint32_t i = 0; i < n; ++i) {
            if ((out[i].flags & ECUIO_SIM_REPLY_PENDING) == 0) {
                return Bytes(out[i].data, out[i].data + out[i].length);
            }
        }
        return {};
    }

    int64_t t = 0;
};

const Bytes kListAf = {0x19, 0x02, 0xAF};

} // namespace

TEST(SimAskTest, EachNodeAnswersItsCodes)
{
    Sim s("mock://a?modules=2");
    const Bytes engine = s.ask(0x7E0, kListAf);
    ASSERT_EQ(engine.size(), 3u + 4 * 4);
    EXPECT_EQ(Bytes(engine.begin(), engine.begin() + 3),
              (Bytes{0x59, 0x02, 0xFF}));
    EXPECT_EQ(s.ask(0x7E0, {0x19, 0x01, 0xAF}),
              (Bytes{0x59, 0x01, 0xFF, 0x00, 0x00, 0x04}));
    EXPECT_EQ(s.ask(0x7E1, {0x19, 0x02, 0xFF}),
              (Bytes{0x59, 0x02, 0xFF, 0x45, 0x61, 0x71, 0x28, 0xC4, 0x01, 0x00,
                     0x24}));
    EXPECT_EQ(s.ask(0x7E1, {0x19, 0x01, 0x08}),
              (Bytes{0x59, 0x01, 0xFF, 0x00, 0x00, 0x01}));
    EXPECT_TRUE(s.ask(0x7DF, kListAf).empty()) << "a functional read: none";
}

TEST(SimAskTest, TheCodesKeyAimsAtOneModule)
{
    {
        Sim s("mock://a?modules=2&codes=refused@7E9");
        EXPECT_EQ(s.ask(0x7E1, kListAf), (Bytes{0x7F, 0x19, 0x11}));
        EXPECT_EQ(s.ask(0x7E0, kListAf).size(), 3u + 4 * 4);
    }
    {
        Sim s("mock://a?modules=2&codes=silent@7E9");
        EXPECT_TRUE(s.ask(0x7E1, kListAf).empty());
        EXPECT_EQ(s.ask(0x7E0, kListAf).size(), 3u + 4 * 4);
    }
    Sim s("mock://a?modules=2&codes=none");
    EXPECT_EQ(s.ask(0x7E0, kListAf), (Bytes{0x59, 0x02, 0xFF}));
    EXPECT_EQ(s.ask(0x7E1, kListAf), (Bytes{0x59, 0x02, 0xFF}));
}

TEST(SimEcuTest, ACodesKeyRefusalNamesTheKeys)
{
    for (const char* uri : {"mock://a?codes=loud", "mock://a?codes=refused@",
                            "mock://a?codes=refused@zz"}) {
        ecuio_sim_t* sim = nullptr;
        EXPECT_EQ(ecuio_sim_create_unbound(uri, 0, &sim), ECUIO_ERR_BAD_URI)
            << uri;
        EXPECT_NE(std::string(ecuio_last_error_detail())
                      .find("isn't refused, none, extended or silent"),
                  std::string::npos)
            << uri << ": " << ecuio_last_error_detail();
        ecuio_sim_destroy(sim);
    }
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create_unbound("mock://a?code=none", 0, &sim),
              ECUIO_ERR_BAD_URI);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("faults, codes,"),
              std::string::npos)
        << ecuio_last_error_detail();
}

TEST(SimAskTest, TheEngineSharesAnEmissionsCodeWithItsJ1979List)
{
    Sim s("mock://a");
    const Bytes stored = s.ask(0x7E0, {0x03});
    ASSERT_GE(stored.size(), 4u);
    EXPECT_EQ(stored[2], 0x03);
    EXPECT_EQ(stored[3], 0x01) << "P0301 stored";
    EXPECT_EQ(s.ask(0x7E0, {0x19, 0x02, 0x08}),
              (Bytes{0x59, 0x02, 0xFF, 0x9A, 0x01, 0x1C, 0x89, 0x03, 0x01, 0x00,
                     0x08}))
        << "B1A01-1C and P0301-00 stored";
}

TEST(SimAskTest, AModuleReadsItsCodesOnlyInTheSessionUnderExtended)
{
    Sim s("mock://a?codes=extended");
    EXPECT_EQ(s.ask(0x7E0, kListAf), (Bytes{0x7F, 0x19, 0x7F}));
    const Bytes entered = s.ask(0x7E0, {0x10, 0x03});
    ASSERT_FALSE(entered.empty());
    ASSERT_EQ(entered[0], 0x50);
    EXPECT_EQ(s.ask(0x7E0, kListAf).size(), 3u + 4 * 4);
}
