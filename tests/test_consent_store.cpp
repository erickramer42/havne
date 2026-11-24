// File/layer: tests/test_consent_store.cpp | test-only, C++17, Qt-free
// Testing: UNIT: ConsentStoreTest (test_core)
//
// The consent store (consent_store.{h,cpp}, consent_store_api.cpp): a
// record made from the plugin, the first second kept, a withdraw of one
// or all, the newest of an id, the file's refusals in words that name no
// path, two opens sharing one owner, a merge with another writer and
// with a removed file, each failing step of a save, and the
// out-of-memory loop.

#include "consent_store.h"
#include "folder_files.h"
#include "partial_left.h"
#include "security_store_support.h" /* Folder: a folder for the case */

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace {

using security_store_test::Folder;

constexpr uint64_t kUtc = 1795132800; /* a made-up second in 2026 */

/* A format 3 data plugin naming its publisher: version moves the
 * fingerprint; publisher "" names none (format 2) */
std::string plugin_text(const std::string& version,
                        const std::string& id = "talker", bool publisher = true)
{
    return std::string("{\"havne_plugin\": ") + (publisher ? "3" : "2") +
           ", \"id\": \"" + id + "\", \"name\": \"Talker\", \"version\": \"" +
           version +
           R"(", "requires": {"core": "0.1.0"}, "source": {"name": "t"}, )"
           R"("tiers": ["data"], )" +
           (publisher
                ? R"("publisher": {"name": "Made-up Tools", )"
                  R"("contact": "https://example.com/help", )"
                  R"("license": "MIT", "terms": "Use at your own risk."}, )"
                : "") +
           R"("ecus": [{"id": "pcm", "rx": "7E8"}], )"
           R"("variants": [{"id": "base", "files": ["a.json"]}], )"
           R"("files": {"a.json": {"havne_signals": 1, "signals": [{"key": )"
           R"("oil", "name": "Oil", "ecu": "pcm", "read": {"service": "22", )"
           R"("number": "F40D"}, "field": {"byte": 0}}]}}})";
}

struct PluginFree {
    void operator()(ecuio_plugin_t* p) const { ecuio_plugin_free(p); }
};
using Plugin = std::unique_ptr<ecuio_plugin_t, PluginFree>;

Plugin plugin(const std::string& version, const std::string& id = "talker",
              bool publisher = true)
{
    const std::string text = plugin_text(version, id, publisher);
    ecuio_plugin_t* p = nullptr;
    EXPECT_EQ(ecuio_plugin_load(text.data(), text.size(), nullptr, nullptr, &p),
              ECUIO_OK)
        << ecuio_last_error_detail();
    return Plugin(p);
}

struct StoreClose {
    void operator()(ecuio_consent_store_t* s) const
    {
        EXPECT_EQ(ecuio_consent_store_close(s), ECUIO_OK);
    }
};
using Store = std::unique_ptr<ecuio_consent_store_t, StoreClose>;

Store open_store(const std::string& path)
{
    ecuio_consent_store_t* s = nullptr;
    EXPECT_EQ(ecuio_consent_store_open(path.c_str(), &s), ECUIO_OK)
        << ecuio_last_error_detail();
    return Store(s);
}

std::string print_of(const ecuio_plugin_t* p)
{
    ecuio_plugin_fingerprint_t f{};
    f.struct_size = sizeof f;
    EXPECT_EQ(ecuio_plugin_fingerprint(p, &f), ECUIO_OK);
    return std::string(reinterpret_cast<const char*>(f.fingerprint), 32);
}

const uint8_t* bytes_of(const std::string& print)
{
    return reinterpret_cast<const uint8_t*>(print.data());
}

ecuio_consent_record_t blank()
{
    ecuio_consent_record_t r;
    std::memset(&r, 0xA5, sizeof r);
    r.struct_size = sizeof r;
    return r;
}

uint32_t count_of(const ecuio_consent_store_t* s)
{
    uint32_t n = 99;
    EXPECT_EQ(ecuio_consent_store_count(s, &n), ECUIO_OK);
    return n;
}

std::string path_in(const Folder& f)
{
    return (f.path / "consent.json").generic_u8string();
}

std::string read_text(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void write_text(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/* A record's text, its fields as given */
std::string record_text(const std::string& fingerprint,
                        const std::string& publisher = "P",
                        const std::string& extra = "")
{
    return R"({"id": "talker", "version": "1.0.0", "fingerprint": ")" +
           fingerprint + R"(", "terms": ")" + std::string(64, 'b') +
           R"(", "publisher": ")" + publisher +
           R"(", "given_utc": 1795132800)" + extra + "}";
}

std::string store_with(const std::string& records)
{
    return R"({"havne_consent_store": 1, "entries": [)" + records + "]}";
}

/* The refusal open gives for text, which must name no path */
std::string open_refusal(const Folder& f, const std::string& text)
{
    write_text(path_in(f), text);
    ecuio_consent_store_t* s = reinterpret_cast<ecuio_consent_store_t*>(1);
    EXPECT_EQ(ecuio_consent_store_open(path_in(f).c_str(), &s),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(s, nullptr);
    const std::string words = ecuio_last_error_detail();
    EXPECT_EQ(words.find(f.path.filename().generic_u8string()),
              std::string::npos)
        << words;
    return words;
}

} // namespace

TEST(ConsentStoreTest, AGiveIsTheRecordOfThePluginItself)
{
    Folder f;
    const Plugin p = plugin("1.0.0");
    {
        const Store s = open_store(path_in(f));
        EXPECT_EQ(count_of(s.get()), 0u) << "no file is an empty store";
        ASSERT_EQ(ecuio_consent_store_give(s.get(), p.get(), kUtc), ECUIO_OK)
            << ecuio_last_error_detail();
    }
    const Store s = open_store(path_in(f));
    ASSERT_EQ(count_of(s.get()), 1u);
    ecuio_consent_record_t r = blank();
    ASSERT_EQ(
        ecuio_consent_store_find(s.get(), bytes_of(print_of(p.get())), &r),
        ECUIO_OK);
    EXPECT_EQ(r.struct_size, sizeof r);
    EXPECT_EQ(r.flags, 0u);
    EXPECT_EQ(r.given_utc, kUtc);
    EXPECT_STREQ(r.id, "talker");
    EXPECT_STREQ(r.version, "1.0.0");
    EXPECT_STREQ(r.publisher, "Made-up Tools");
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(r.fingerprint), 32),
              print_of(p.get()));
    ecuio_plugin_publisher_t pub{};
    pub.struct_size = sizeof pub;
    ASSERT_EQ(ecuio_plugin_publisher(p.get(), &pub), ECUIO_OK);
    EXPECT_EQ(std::memcmp(r.terms_sha256, pub.terms_sha256, 32), 0);
    for (const uint32_t x : r.reserved) {
        EXPECT_EQ(x, 0u);
    }

    /* Given again later: the first second stands */
    ASSERT_EQ(ecuio_consent_store_give(s.get(), p.get(), kUtc + 60), ECUIO_OK);
    r = blank();
    ASSERT_EQ(
        ecuio_consent_store_find(s.get(), bytes_of(print_of(p.get())), &r),
        ECUIO_OK);
    EXPECT_EQ(r.given_utc, kUtc);
    EXPECT_EQ(count_of(s.get()), 1u);

    /* Another version's fingerprint has none */
    const Plugin later = plugin("1.1.0");
    r = blank();
    EXPECT_EQ(
        ecuio_consent_store_find(s.get(), bytes_of(print_of(later.get())), &r),
        ECUIO_ERR_CONSENT_REQUIRED);
    EXPECT_EQ(r.given_utc, 0xA5A5A5A5A5A5A5A5u) << "out untouched";
}

TEST(ConsentStoreTest, AGiveIsRefusedWhatItCantKeep)
{
    Folder f;
    const Store s = open_store(path_in(f));
    const Plugin none = plugin("1.0.0", "talker", false);
    EXPECT_EQ(ecuio_consent_store_give(s.get(), none.get(), kUtc),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("names no publisher"),
              std::string::npos);
    const Plugin p = plugin("1.0.0");
    EXPECT_EQ(ecuio_consent_store_give(s.get(), p.get(), 0),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_consent_store_give(s.get(), p.get(), 253402300800u),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_consent_store_give(nullptr, p.get(), kUtc),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_consent_store_give(s.get(), nullptr, kUtc),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(count_of(s.get()), 0u);
    EXPECT_FALSE(std::ifstream(path_in(f)).good()) << "nothing written";
}

TEST(ConsentStoreTest, AWithdrawTakesOneOrEveryRecordOfAnId)
{
    Folder f;
    const Store s = open_store(path_in(f));
    const Plugin v1 = plugin("1.0.0");
    const Plugin v2 = plugin("1.1.0");
    const Plugin other = plugin("1.0.0", "other");
    ASSERT_EQ(ecuio_consent_store_give(s.get(), v1.get(), kUtc), ECUIO_OK);
    ASSERT_EQ(ecuio_consent_store_give(s.get(), v2.get(), kUtc + 10), ECUIO_OK);
    ASSERT_EQ(ecuio_consent_store_give(s.get(), other.get(), kUtc + 5),
              ECUIO_OK);

    /* The newest of an id, and the list newest first */
    ecuio_consent_record_t r = blank();
    ASSERT_EQ(ecuio_consent_store_latest(s.get(), "talker", &r), ECUIO_OK);
    EXPECT_STREQ(r.version, "1.1.0");
    const char* order[] = {"1.1.0", "1.0.0", "1.0.0"};
    const char* ids[] = {"talker", "other", "talker"};
    for (uint32_t i = 0; i < 3; ++i) {
        r = blank();
        ASSERT_EQ(ecuio_consent_store_get(s.get(), i, &r), ECUIO_OK);
        EXPECT_STREQ(r.version, order[i]) << i;
        EXPECT_STREQ(r.id, ids[i]) << i;
    }
    r = blank();
    EXPECT_EQ(ecuio_consent_store_get(s.get(), 3, &r),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_consent_store_latest(s.get(), "nobody", &r),
              ECUIO_ERR_CONSENT_REQUIRED);

    ASSERT_EQ(ecuio_consent_store_withdraw(s.get(), "talker",
                                           bytes_of(print_of(v1.get()))),
              ECUIO_OK);
    EXPECT_EQ(count_of(s.get()), 2u);
    ASSERT_EQ(ecuio_consent_store_withdraw(s.get(), "talker", nullptr),
              ECUIO_OK);
    EXPECT_EQ(count_of(s.get()), 1u);
    EXPECT_EQ(ecuio_consent_store_withdraw(s.get(), "talker", nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    /* A fingerprint of another id withdraws nothing */
    EXPECT_EQ(ecuio_consent_store_withdraw(s.get(), "talker",
                                           bytes_of(print_of(other.get()))),
              ECUIO_ERR_INVALID_ARGUMENT);

    const Store again = open_store(path_in(f));
    EXPECT_EQ(count_of(again.get()), 1u);
}

TEST(ConsentStoreTest, AFileThatDoesntReadIsRefusedInWordsThatNameNoPath)
{
    Folder f;
    const std::string fp(64, 'a');
    struct row {
        const char* what;
        std::string text;
        const char* words;
    };
    const row rows[] = {
        {"not JSON", "{", "the consent store doesn't read (line 1"},
        {"a later format", R"({"havne_consent_store": 2, "entries": []})",
         "this Havne reads a consent store of format 1 only"},
        {"no format", R"({"entries": []})", "needs \"havne_consent_store\""},
        {"a key it doesn't take",
         R"({"havne_consent_store": 1, "entries": [], "x": 1})", "x"},
        {"a record key it doesn't take",
         store_with(record_text(fp, "P", R"(, "key": "")")), "key"},
        {"a short fingerprint", store_with(record_text(std::string(63, 'a'))),
         "a record's fingerprint is 63 bytes"},
        {"an upper-case fingerprint",
         store_with(record_text(std::string(64, 'A'))),
         "a record's fingerprint takes 64 of 0-9 and a-f"},
        {"a fingerprint twice",
         store_with(record_text(fp) + ", " + record_text(fp)),
         "a record's fingerprint is given twice"},
        {"a publisher with a tab", store_with(record_text(fp, "a\\tb")),
         "a record's publisher holds a control character"},
        {"a publisher past its bound",
         store_with(record_text(fp, std::string(128, 'p'))),
         "a record's publisher is 128 bytes"},
        {"a second of 0",
         store_with(R"({"id": "talker", "version": "1.0.0", "fingerprint": ")" +
                    fp + R"(", "terms": ")" + fp +
                    R"(", "publisher": "P", "given_utc": 0})"),
         "a record's given_utc takes a whole number from 1"},
        {"a version that isn't X.Y.Z",
         store_with(R"({"id": "talker", "version": "1.0", "fingerprint": ")" +
                    fp + R"(", "terms": ")" + fp +
                    R"(", "publisher": "P", "given_utc": 1})"),
         "a record's version isn't X.Y.Z"},
    };
    for (const row& r : rows) {
        const std::string words = open_refusal(f, r.text);
        EXPECT_NE(words.find(r.words), std::string::npos)
            << r.what << ": " << words;
        EXPECT_NE(words.find("removing the file forgets them"),
                  std::string::npos)
            << r.what;
    }

    /* 257 records are past the bound; 256 read */
    std::string list;
    for (int i = 0; i < 257; ++i) {
        char hex[65];
        std::snprintf(hex, sizeof hex, "%064x", i);
        list += (i > 0 ? ", " : "") + record_text(hex);
        if (i == 255) {
            write_text(path_in(f), store_with(list));
            const Store s = open_store(path_in(f));
            EXPECT_EQ(count_of(s.get()), 256u);
        }
    }
    EXPECT_NE(open_refusal(f, store_with(list)).find("more than 256"),
              std::string::npos);

    /* A file there that can't be opened is refused too */
    std::remove(path_in(f).c_str());
    ecuio_consent::store_test_open_errno = EACCES;
    ecuio_consent_store_t* s = nullptr;
    EXPECT_EQ(ecuio_consent_store_open(path_in(f).c_str(), &s),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(std::string(ecuio_last_error_detail())
                  .find("the consent store can't be opened"),
              std::string::npos);
}

TEST(ConsentStoreTest, TwoOpensOfAPathShareOneStore)
{
    Folder f;
    const Store a = open_store(path_in(f));
    const Store b = open_store(path_in(f));
    const Plugin p = plugin("1.0.0");
    ASSERT_EQ(ecuio_consent_store_give(a.get(), p.get(), kUtc), ECUIO_OK);
    EXPECT_EQ(count_of(b.get()), 1u);
}

TEST(ConsentStoreTest, ASaveMergesAnotherWritersRecords)
{
    Folder f;
    const std::string path = path_in(f);
    const Plugin v1 = plugin("1.0.0");
    const Plugin v2 = plugin("1.1.0");
    const Plugin other = plugin("1.0.0", "other");
    const Store s = open_store(path);
    ASSERT_EQ(ecuio_consent_store_give(s.get(), v1.get(), kUtc), ECUIO_OK);
    ASSERT_EQ(ecuio_consent_store_give(s.get(), v2.get(), kUtc), ECUIO_OK);

    /* Another program gives other and withdraws v2 */
    ecuio_consent::store them;
    std::string why;
    ASSERT_EQ(ecuio_consent::store_load(path, &them, &why),
              ecuio_consent::store_io::ok);
    ASSERT_EQ(them.records.size(), 2u);
    ecuio_consent::record r = them.records[1];
    std::memcpy(r.fingerprint, bytes_of(print_of(other.get())), 32);
    r.id = "other";
    them.records[1] = r; /* v2 out, other in */
    ASSERT_EQ(ecuio_consent::store_save(path, &them, &why),
              ecuio_consent::store_io::ok)
        << why;

    /* This store withdraws v1: other stays, v2 stays withdrawn */
    ASSERT_EQ(ecuio_consent_store_withdraw(s.get(), "talker",
                                           bytes_of(print_of(v1.get()))),
              ECUIO_OK);
    ecuio_consent::store after;
    ASSERT_EQ(ecuio_consent::store_load(path, &after, &why),
              ecuio_consent::store_io::ok);
    ASSERT_EQ(after.records.size(), 1u);
    EXPECT_EQ(after.records[0].id, "other");
    EXPECT_EQ(count_of(s.get()), 1u) << "the store holds what it wrote";
}

/* The file removed between two saves: the records it held stay removed,
 * and only what this store gave since is written */
TEST(ConsentStoreTest, AFileRemovedBetweenTwoSavesBringsNothingBack)
{
    Folder f;
    const std::string path = path_in(f);
    const Plugin v1 = plugin("1.0.0");
    const Plugin v2 = plugin("1.1.0");
    {
        const Store s = open_store(path);
        ASSERT_EQ(ecuio_consent_store_give(s.get(), v1.get(), kUtc), ECUIO_OK);
    }
    ecuio_consent::store s;
    std::string why;
    ASSERT_EQ(ecuio_consent::store_load(path, &s, &why),
              ecuio_consent::store_io::ok);
    ASSERT_EQ(s.records.size(), 1u);
    ASSERT_EQ(std::remove(path.c_str()), 0);

    ecuio_consent::record r = s.records[0];
    std::memcpy(r.fingerprint, bytes_of(print_of(v2.get())), 32);
    r.version = "1.1.0";
    s.records.push_back(r);
    ASSERT_EQ(ecuio_consent::store_save(path, &s, &why),
              ecuio_consent::store_io::ok)
        << why;
    ecuio_consent::store after;
    ASSERT_EQ(ecuio_consent::store_load(path, &after, &why),
              ecuio_consent::store_io::ok);
    ASSERT_EQ(after.records.size(), 1u);
    EXPECT_EQ(after.records[0].version, "1.1.0");
    EXPECT_EQ(s.records.size(), 1u) << "the store holds what it wrote";
}

TEST(ConsentStoreTest, EachFailingStepOfASaveLeavesTheStoreAsItWas)
{
    Folder f;
    const std::string path = path_in(f);
    const Plugin v1 = plugin("1.0.0");
    const Plugin v2 = plugin("1.1.0");
    const Store s = open_store(path);
    ASSERT_EQ(ecuio_consent_store_give(s.get(), v1.get(), kUtc), ECUIO_OK);
    const std::string before = read_text(path);
    for (int step = 1; step <= 3; ++step) {
        ecuio_consent::store_test_fail_at = step;
        EXPECT_EQ(ecuio_consent_store_give(s.get(), v2.get(), kUtc),
                  ECUIO_ERR_LOAD_FAILED)
            << step;
        EXPECT_NE(std::string(ecuio_last_error_detail())
                      .find("the consent store can't be written"),
                  std::string::npos)
            << step;
        EXPECT_EQ(read_text(path), before) << step;
        EXPECT_EQ(count_of(s.get()), 1u) << step;
        EXPECT_FALSE(partial_left(f.path, "consent.json")) << step;
    }
    /* The folder's flush after the replace: the new file stands */
    ecuio_consent::store_test_fail_at = 4;
    EXPECT_EQ(ecuio_consent_store_give(s.get(), v2.get(), kUtc),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(count_of(s.get()), 2u);
    EXPECT_NE(read_text(path), before);

    /* Another holding the lock past the wait */
    ecuio_files::file_lock held;
    std::string words;
    ASSERT_EQ(ecuio_files::lock_file(path + ".lock", 0, &held, &words),
              ecuio_files::lock_got::locked);
    ecuio_consent::store_test_lock_wait_ms = 20;
    EXPECT_EQ(ecuio_consent_store_withdraw(s.get(), "talker", nullptr),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_NE(std::string(ecuio_last_error_detail())
                  .find("another program is using the consent store"),
              std::string::npos);
    ecuio_consent::store_test_lock_wait_ms = 0;
    EXPECT_EQ(count_of(s.get()), 2u);
}

TEST(ConsentStoreTest, OutOfMemoryAnywhereIsAnsweredNotThrown)
{
    Folder f;
    const std::string path = path_in(f);
    const Plugin v1 = plugin("1.0.0");
    const Plugin v2 = plugin("1.1.0");
    {
        const Store s = open_store(path);
        ASSERT_EQ(ecuio_consent_store_give(s.get(), v1.get(), kUtc), ECUIO_OK);
    }
    const std::string one = read_text(path);
    const std::string print1 = print_of(v1.get()); /* allocated outside */
    unsigned long refused = 0;
    for (unsigned long n = 1;; ++n) {
        write_text(path, one);
        ecuio_consent_store_t* s = nullptr;
        fail_new_on_this_thread(n);
        ecuio_status_t open = ecuio_consent_store_open(path.c_str(), &s);
        ecuio_status_t give = ECUIO_ERR_OUT_OF_MEMORY;
        ecuio_status_t take = ECUIO_ERR_OUT_OF_MEMORY;
        if (open == ECUIO_OK) {
            give = ecuio_consent_store_give(s, v2.get(), kUtc);
            if (give == ECUIO_OK) {
                take =
                    ecuio_consent_store_withdraw(s, "talker", bytes_of(print1));
            }
        }
        fail_new_on_this_thread(0);
        for (const ecuio_status_t st : {open, give, take}) {
            ASSERT_TRUE(st == ECUIO_OK || st == ECUIO_ERR_OUT_OF_MEMORY)
                << n << ": " << st;
        }
        if (open == ECUIO_OK) {
            const uint32_t count = count_of(s);
            EXPECT_EQ(ecuio_consent_store_close(s), ECUIO_OK);
            /* Whole either way: a refused step changed nothing */
            ecuio_consent::store back;
            std::string why;
            ASSERT_EQ(ecuio_consent::store_load(path, &back, &why),
                      ecuio_consent::store_io::ok)
                << n;
            EXPECT_EQ(back.records.size(), count) << n;
            EXPECT_FALSE(partial_left(f.path, "consent.json")) << n;
        } else {
            EXPECT_EQ(s, nullptr) << n;
        }
        if (take == ECUIO_OK) {
            break;
        }
        ++refused;
        ASSERT_LT(n, 5000u);
    }
    EXPECT_GT(refused, 3u);
}
