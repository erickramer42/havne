// File/layer: tests/test_scrub_mask.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ScrubMaskTest (test_core)
//
// scrub.h's check with kMask: a VIN-shaped token in text a user wrote is
// masked, its span clipped to its run of the VIN alphabet, the bytes fed in
// pieces as an export's flush feeds them; every masked text checked again
// finds nothing; an identity value still fails; JSON's escapes separate
// runs; window_in judges a message by the window rule alone. The VINs are
// made up (FAKE).

#include "test_export_support.h"

namespace {

namespace sc = ecuio_scrub;

const uint32_t kShaped = ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED + 1;

struct Masked {
    std::string text;
    uint32_t found = 0;
    uint64_t masked = 0;
};

/* text through a check of mode, piece bytes at a time, written as an
 * export's flush writes: the last kMaskReach bytes held to the next */
Masked masked(const std::string& text, size_t piece = SIZE_MAX,
              uint32_t mode = sc::kMask, const sc::values* v = nullptr)
{
    const sc::values none;
    sc::check c(v != nullptr ? *v : none, mode);
    Masked m;
    std::string buf;
    size_t fed = 0;
    for (size_t at = 0; at < text.size(); at += piece) {
        buf += text.substr(at, piece);
        c.feed_masking(reinterpret_cast<uint8_t*>(&buf[0]) + fed,
                       buf.size() - fed, fed);
        const size_t keep = (std::min)(sc::kMaskReach, buf.size());
        c.written(reinterpret_cast<const uint8_t*>(buf.data()),
                  buf.size() - keep);
        m.text += buf.substr(0, buf.size() - keep);
        buf.erase(0, buf.size() - keep);
        fed = keep;
        if (piece == SIZE_MAX) {
            break;
        }
    }
    c.finish_masking(reinterpret_cast<uint8_t*>(&buf[0]) + buf.size(),
                     buf.size());
    c.written(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
    m.text += buf;
    m.found = c.finish();
    m.masked = c.masked();
    return m;
}

uint32_t refused(const std::string& text, uint32_t mode = sc::kRefuse)
{
    const sc::values none;
    sc::check c(none, mode);
    c.feed(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    return c.finish();
}

/* the mask as a reader sees it, never kMaskChar's value read back */
std::string stars(size_t n)
{
    return std::string(n, '*');
}

/* A masked text checked again finds nothing and masks nothing more */
void expect_clean(const std::string& text, uint32_t mode = sc::kMask)
{
    EXPECT_EQ(refused(text, mode & sc::kJson), 0u) << text;
    const Masked again = masked(text, SIZE_MAX, mode);
    EXPECT_EQ(again.text, text);
    EXPECT_EQ(again.masked, 0u);
}

TEST(ScrubMaskTest, AWindowInARunIsMaskedAndItsNeighboursOutsideTheAlphabetKept)
{
    const std::string in = std::string("a") + kVin + "b";
    EXPECT_EQ(refused(in), kShaped); /* refused without kMask */
    const Masked m = masked(in);
    EXPECT_EQ(m.found, 0u);
    EXPECT_EQ(m.masked, 1u);
    EXPECT_EQ(m.text, "a" + stars(17) + "b");
    expect_clean(m.text);
}

/* A hit shifted from the VIN it overlaps masks every character of it:
 * ZZZFAKE12M0000001's own ninth isn't its check digit, but a window one
 * before it (B...) or two after it (...AH) has one */
TEST(ScrubMaskTest, AShiftedHitMasksTheWholeVinItOverlaps)
{
    const std::string vin = "ZZZFAKE12M0000001";
    EXPECT_EQ(refused(vin + "x"), 0u); /* no window of its own */
    for (const std::string& in : {"B" + vin, vin + "AH", "x B" + vin + " y"}) {
        SCOPED_TRACE(in);
        const Masked m = masked(in);
        EXPECT_EQ(m.found, 0u);
        EXPECT_EQ(m.masked, 1u);
        EXPECT_EQ(m.text.find_first_not_of("*x y"), std::string::npos)
            << m.text;
        expect_clean(m.text);
    }
}

/* The span reaches 16 either side of the window and no further */
TEST(ScrubMaskTest, ARunLongerThanTheSpanKeepsWhatsPastIt)
{
    const std::string in = std::string(20, '0') + kVin + std::string(20, '5');
    const Masked m = masked(in);
    EXPECT_EQ(m.found, 0u);
    EXPECT_EQ(m.masked, 1u);
    EXPECT_EQ(m.text, "0000" + stars(16 + 17 + 16) + "5555");
    expect_clean(m.text);
}

/* What a span leaves of its run is a run of its own, as written: 17 of
 * the alphabet with the shape there is masked too, before or after */
TEST(ScrubMaskTest, ARunOfSeventeenLeftBesideASpanIsMasked)
{
    const std::string left = "WWWFAKE33M0000003"; /* no window of its own */
    EXPECT_EQ(refused(left + "a"), 0u);
    const Masked before = masked(left + std::string(16, '3') + kVin + ".");
    EXPECT_EQ(before.found, 0u);
    EXPECT_EQ(before.masked, 2u);
    EXPECT_EQ(before.text, stars(17 + 16 + 17) + ".");
    expect_clean(before.text);

    const Masked after =
        masked(std::string(kVin) + std::string(16, '3') + left + ".");
    EXPECT_EQ(after.found, 0u);
    EXPECT_EQ(after.masked, 2u);
    EXPECT_EQ(after.text, stars(17 + 16 + 17) + ".");
    expect_clean(after.text);

    /* sixteen left, or eighteen, is no VIN */
    const Masked short_left = masked("3" + std::string(kVin).substr(1) + "a");
    EXPECT_EQ(short_left.masked, 0u);
}

TEST(ScrubMaskTest, ARunOfExactlySeventeenIsMaskedWhole)
{
    /* the fake VIN, whose window the rule takes too, and a token it
     * doesn't (no check digit, no listed maker): the shape alone */
    const std::string other = "WWWFAKE33M0000003";
    ASSERT_FALSE(sc::check::window_in(
        reinterpret_cast<const uint8_t*>(("Z" + other).data()), 18));
    for (const std::string& token : {std::string(kVin), other}) {
        for (const std::string& in :
             {token, "VIN: " + token + ", next",
              std::string("\x01") + token + "\x02"}) {
            SCOPED_TRACE(in);
            EXPECT_EQ(refused(in), kShaped);
            const Masked m = masked(in);
            EXPECT_EQ(m.found, 0u);
            EXPECT_EQ(m.masked, 1u);
            std::string want = in;
            want.replace(in.find(token), 17, stars(17));
            EXPECT_EQ(m.text, want);
            expect_clean(m.text);
        }
    }
    /* not the shape: kept */
    for (const char* in : {"ZZZFAKE11M000000", "zzzfake11m0000001",
                           "0123456789ABCDEF0", "ZZZFAKEI1M0000001"}) {
        EXPECT_EQ(masked(in).text, in);
    }
}

TEST(ScrubMaskTest, TwoHitsOfOneRunApartAreTwoSpans)
{
    const std::string gap(40, 'x');
    const std::string in = std::string(kVin) + gap + kVin;
    const Masked m = masked(in);
    EXPECT_EQ(m.found, 0u);
    EXPECT_EQ(m.masked, 2u);
    EXPECT_EQ(m.text, stars(17) + gap + stars(17));
    expect_clean(m.text);
}

/* The held bytes cover every mask: fed in pieces of every size, the
 * bytes written are one piece's */
TEST(ScrubMaskTest, PiecesOfEverySizeMaskAsOnePiece)
{
    const std::string inputs[] = {
        "row,a" + std::string(kVin) + "b,1\n",
        std::string(20, '0') + kVin + std::string(20, '5') + "\n",
        "WWWFAKE33M0000003" + std::string(16, '3') + kVin + ";",
        "name," + std::string(kVin) + ",unit\n" + std::string(30, '-') +
            "BZZZFAKE12M0000001",
    };
    for (const std::string& one : inputs) {
        const std::string in = std::string(70, '.') + one + one;
        const Masked whole = masked(in);
        ASSERT_EQ(whole.found, 0u);
        for (size_t piece = 1; piece <= 64; ++piece) {
            SCOPED_TRACE(piece);
            const Masked m = masked(in, piece);
            EXPECT_EQ(m.text, whole.text);
            EXPECT_EQ(m.masked, whole.masked);
            EXPECT_EQ(m.found, 0u);
        }
    }
}

/* A mask that can't reach its bytes fails the check: fed with feed, or
 * past the bytes held */
TEST(ScrubMaskTest, AMaskPastTheHeldBytesFailsTheCheck)
{
    const sc::values none;
    sc::check fed(none, sc::kMask);
    const std::string in = std::string("a") + kVin + "b";
    fed.feed(reinterpret_cast<const uint8_t*>(in.data()), in.size());
    EXPECT_EQ(fed.finish(), kShaped);

    sc::check held(none, sc::kMask);
    std::string first = "a" + std::string(kVin).substr(0, 10);
    std::string second = std::string(kVin).substr(10) + "b";
    held.feed_masking(reinterpret_cast<uint8_t*>(&first[0]), first.size(), 0);
    held.feed_masking(reinterpret_cast<uint8_t*>(&second[0]), second.size(),
                      0); /* the first piece written, none held */
    EXPECT_EQ(held.finish_masking(
                  reinterpret_cast<uint8_t*>(&second[0]) + second.size(), 0),
              kShaped);
}

/* The recording's own values are found first and fail the check: a mask
 * never hides one */
TEST(ScrubMaskTest, AnIdentityValueStillFails)
{
    sc::values own;
    ASSERT_TRUE(own.add(kVin, 17, ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION));
    const uint32_t ident = ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION + 1;
    EXPECT_EQ(masked(std::string("a") + kVin + "b", SIZE_MAX, sc::kMask, &own)
                  .found,
              ident);
    EXPECT_EQ(masked(std::string(",") + kVin + ",", SIZE_MAX, sc::kMask, &own)
                  .found,
              ident);
    EXPECT_EQ(masked(std::string(",") + kVin + ",", 5, sc::kMask, &own).found,
              ident);
}

/* A value holding the mask's character would be made by a mask: the bytes
 * as written are searched for it */
TEST(ScrubMaskTest, AValueAMaskWouldMakeFails)
{
    sc::values starred;
    const std::string made = "x" + stars(17) + "y";
    ASSERT_TRUE(starred.add(made.data(), made.size(),
                            ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES));
    EXPECT_TRUE(starred.starred());
    const std::string in = std::string("x") + kVin + "y";
    EXPECT_EQ(masked(in, SIZE_MAX, sc::kMask, &starred).found,
              ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES + 1);
    EXPECT_EQ(masked(in, 3, sc::kMask, &starred).found,
              ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES + 1);

    sc::values plain;
    ASSERT_TRUE(plain.add("FAKECAL-01", 10, ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES));
    EXPECT_FALSE(plain.starred());
}

/* In JSON an escape stands for one character: one that isn't a letter or
 * a digit separates runs there as it does in signals.csv, and is never
 * masked */
TEST(ScrubMaskTest, JsonEscapesSeparateRunsAsTheirCharacters)
{
    const std::string bs = "\\";
    for (const std::string& esc :
         {bs + "u00e9", bs + "n", bs + "t", bs + "\"", bs + bs, bs + "/"}) {
        SCOPED_TRACE(esc);
        const std::string in = "{\"name\": \"" + esc + kVin + esc + "\"}";
        EXPECT_EQ(refused(in, sc::kJson), kShaped);
        const Masked m = masked(in, SIZE_MAX, sc::kMask | sc::kJson);
        EXPECT_EQ(m.found, 0u);
        EXPECT_EQ(m.masked, 1u);
        EXPECT_EQ(m.text, "{\"name\": \"" + esc + stars(17) + esc + "\"}");
        expect_clean(m.text, sc::kMask | sc::kJson);
    }
    /* outside JSON the escape's letter joins the run: no run of 17 (this
     * VIN's ninth isn't its check digit) */
    const std::string own = "\"" + bs + "nZZZFAKE12M0000001\"";
    EXPECT_EQ(refused(own, sc::kRefuse), 0u);
    EXPECT_EQ(refused(own, sc::kJson), kShaped);
    /* an escape of a letter or digit reads as its bytes, as outside JSON */
    const std::string letter = "\"" + bs + "u0041" + kVin + "\"";
    EXPECT_EQ(refused(letter, sc::kJson), refused(letter, sc::kRefuse));
    /* an escape cut short reads as its bytes */
    const std::string cut = "\"" + bs + "u00" + kVin + "\"";
    EXPECT_EQ(refused(cut, sc::kJson), refused(cut, sc::kRefuse));
    /* fed in pieces, an escape across them */
    const std::string in = "[\"" + bs + "u00e9" + kVin + "\"]";
    const Masked whole = masked(in, SIZE_MAX, sc::kMask | sc::kJson);
    for (size_t piece = 1; piece <= 8; ++piece) {
        EXPECT_EQ(masked(in, piece, sc::kMask | sc::kJson).text, whole.text);
    }
}

TEST(ScrubMaskTest, WindowInIsTheWindowRuleAlone)
{
    auto window_in = [](const std::string& s) {
        return sc::check::window_in(reinterpret_cast<const uint8_t*>(s.data()),
                                    s.size());
    };
    EXPECT_TRUE(window_in(std::string("\x62\xD8\x01") + "a" + kVin));
    EXPECT_TRUE(window_in("BZZZFAKE12M0000001"));
    EXPECT_FALSE(window_in(std::string("\x62\xD8\x01") + kVin)); /* the shape */
    EXPECT_FALSE(window_in(std::string(kVin) + " " + kVin));
    EXPECT_FALSE(window_in("ZZZFAKE12M0000001"));
    EXPECT_FALSE(window_in(""));
}

} // namespace
