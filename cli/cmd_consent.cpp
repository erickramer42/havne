// File/layer: cli/cmd_consent.cpp | havne-cli, plugins terms and plugins
//             consent (ADR-0066's appendix of 2026-11-23)
// Testing: HARNESS: CliConsentTest (tests/cli_consent.py), CliUsageTest
//          (ctest)
//
// What a person reads before consenting, and the consent store's words:
// `plugins terms <pkg> [--json]` prints Havne's lines, the publisher block,
// the terms and the full fingerprint; `plugins consent <pkg> --give <fp>`
// prints them again, then records the consent; `plugins consent --list`
// and `--withdraw <id> [<fp>]` read and change the store. The store is
// --consent-store's, or the app's consent.json (settings_home.h). Public
// headers only.

#include "consent_lines.h"
#include "settings_home.h"

#include "commands.h"
#include "profile_source.h"
#include "common.h"

#include "ecuio/plugin.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

namespace cmd {

namespace {

constexpr const char* kStoreOption = "--consent-store";

// The store's path: the option's, or the app's consent.json; "" with words
// on standard error when no folder is known
std::string store_path(const char* named)
{
    if (named != nullptr) {
        return named;
    }
    const std::string path = cli::settings_file("consent.json");
    if (path.empty()) {
        std::fprintf(stderr, "havne-cli: no settings folder is known: name "
                             "the store with --consent-store\n");
    }
    return path;
}

// The default store's folder, made when it isn't there (the app makes it
// at its first run)
void make_home(const char* named)
{
    if (named == nullptr) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::u8path(cli::settings_home()), ec);
    }
}

void print_text(const std::string& text)
{
    std::fwrite(text.data(), 1, text.size(), stdout);
}

// The package loaded and its facts; false with words on standard error
bool load_facts(const char* package, ecuio_plugin_t** p, cli::ConsentFacts* f)
{
    std::string why;
    if (load_package(package, p, &why) != ECUIO_OK) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        return false;
    }
    if (!cli::consent_facts(*p, f)) {
        std::fprintf(stderr, "havne-cli: %s\n", ecuio_last_error_detail());
        return false;
    }
    return true;
}

int run_plugins_terms(const char* package, bool json)
{
    ecuio_plugin_t* p = nullptr;
    cli::ConsentFacts f;
    if (!load_facts(package, &p, &f)) {
        ecuio_plugin_free(p);
        return kExitConnect;
    }
    int code = kExitOk;
    if (json) {
        Output out;
        write_start(out);
        BigLine b(cli::terms_line_bytes(f));
        cli::terms_line(b.line, 0, f);
        write_line(out, b.line);
        code = ended(out, kExitOk);
    } else if (!f.has_publisher) {
        print_text(std::string(cli::kNoPublisher) + "\n");
    } else {
        print_text(cli::terms_text(f, package, nullptr));
    }
    ecuio_plugin_free(p);
    std::fflush(stdout);
    return code;
}

// --give's fingerprint against the plugin's: 0 to go on, else exit 2 with
// the words' refusal
int check_given(const cli::ConsentFacts& f, const char* given,
                const char* package)
{
    uint8_t typed[32] = {};
    if (!cli::parse_fingerprint(given, typed)) {
        std::fprintf(stderr, "havne-cli: --give takes the 64-digit "
                             "fingerprint `plugins terms` prints.\n");
        return kExitUsage;
    }
    if (std::memcmp(typed, f.fingerprint, sizeof typed) != 0) {
        std::fprintf(stderr,
                     "havne-cli: --give does not match this plugin (its "
                     "fingerprint begins %s). Run: havne-cli plugins terms "
                     "%s\n",
                     cli::hex_text(f.fingerprint, 8).c_str(), package);
        return kExitUsage;
    }
    return 0;
}

int run_plugins_give(const char* package, const char* given, const char* named)
{
    ecuio_plugin_t* p = nullptr;
    cli::ConsentFacts f;
    if (!load_facts(package, &p, &f)) {
        ecuio_plugin_free(p);
        return kExitConnect;
    }
    int code = 0;
    if (!f.has_publisher) {
        std::fprintf(stderr, "havne-cli: %s, and takes no consent\n",
                     cli::kNoPublisher);
        code = kExitUsage;
    }
    code = code != 0 ? code : check_given(f, given, package);
    const std::string path = code != 0 ? "" : store_path(named);
    if (code == 0 && path.empty()) {
        code = kExitUsage;
    }
    if (code != 0) {
        ecuio_plugin_free(p);
        return code;
    }
    print_text(cli::terms_text(f, package, named));
    std::fflush(stdout);
    make_home(named);
    ecuio_consent_store_t* store = nullptr;
    ecuio_status_t st = ecuio_consent_store_open(path.c_str(), &store);
    if (st == ECUIO_OK) {
        st = ecuio_consent_store_give(
            store, p, static_cast<uint64_t>(std::time(nullptr)));
    }
    if (st != ECUIO_OK) {
        std::fprintf(stderr, "havne-cli: the consent wasn't given: %s\n",
                     ecuio_last_error_detail());
        code = kExitConnect;
    } else {
        print_text("\nAllowed: " + std::string(f.info.id) + " " +
                   f.info.version + ", fingerprint " +
                   cli::hex_text(f.fingerprint, 32) + "\n");
    }
    ecuio_consent_store_close(store);
    ecuio_plugin_free(p);
    std::fflush(stdout);
    return code;
}

// The store opened for --list and --withdraw: start, and an error line
// with exit 3 when it can't be
int open_listed(Output& out, const char* named, ecuio_consent_store_t** store)
{
    const std::string path = store_path(named);
    if (path.empty()) {
        return kExitUsage;
    }
    write_start(out);
    const ecuio_status_t st = ecuio_consent_store_open(path.c_str(), store);
    if (st != ECUIO_OK) {
        write_error(out, st, ecuio_last_error_detail());
        return ended(out, kExitConnect);
    }
    return 0;
}

int run_plugins_list_consents(const char* named)
{
    Output out;
    ecuio_consent_store_t* store = nullptr;
    const int opened = open_listed(out, named, &store);
    if (opened != 0) {
        return opened;
    }
    uint32_t count = 0;
    (void)ecuio_consent_store_count(store, &count);
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_consent_record_t r{};
        r.struct_size = sizeof r;
        if (ecuio_consent_store_get(store, i, &r) != ECUIO_OK) {
            continue;
        }
        BigLine b(2048);
        cli::consent_line(b.line, r);
        write_line(out, b.line);
    }
    ecuio_consent_store_close(store);
    return ended(out, kExitOk);
}

int run_plugins_withdraw(const char* id, const char* fingerprint,
                         const char* named)
{
    uint8_t print[32] = {};
    if (fingerprint != nullptr && !cli::parse_fingerprint(fingerprint, print)) {
        std::fprintf(stderr, "havne-cli: --withdraw takes a plugin id and "
                             "optionally its 64-digit fingerprint\n");
        return kExitUsage;
    }
    Output out;
    ecuio_consent_store_t* store = nullptr;
    make_home(named);
    const int opened = open_listed(out, named, &store);
    if (opened != 0) {
        return opened;
    }
    const ecuio_status_t st = ecuio_consent_store_withdraw(
        store, id, fingerprint != nullptr ? print : nullptr);
    int code = kExitOk;
    if (st != ECUIO_OK) {
        write_error(out, st, ecuio_last_error_detail());
        code = kExitConnect;
    } else {
        BigLine b(1024);
        b.line.raw("{\"type\":\"withdrawn\"");
        b.line.key("id");
        b.line.string(id, std::strlen(id));
        b.line.key("fingerprint");
        if (fingerprint != nullptr) {
            const std::string hex = cli::hex_text(print, 32);
            b.line.string(hex.data(), hex.size());
        } else {
            b.line.raw("null");
        }
        b.line.ch('}');
        write_line(out, b.line);
    }
    ecuio_consent_store_close(store);
    return ended(out, code);
}

// What `plugins consent` was asked
struct ConsentArgs {
    const char* package = nullptr;
    const char* give = nullptr;
    const char* store = nullptr;
    const char* withdraw = nullptr;
    const char* withdraw_print = nullptr;
    bool list = false;
};

// argv from 3 into a; false for a form `plugins consent` doesn't take
bool parse_consent(int argc, char** argv, ConsentArgs* a)
{
    for (int i = 3; i < argc; ++i) {
        const std::string w = argv[i];
        const bool value = i + 1 < argc && argv[i + 1][0] != '\0';
        if (w == "--give" && value && a->give == nullptr) {
            a->give = argv[++i];
        } else if (w == kStoreOption && value && a->store == nullptr) {
            a->store = argv[++i];
        } else if (w == "--list" && !a->list) {
            a->list = true;
        } else if (w == "--withdraw" && value && a->withdraw == nullptr) {
            a->withdraw = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                a->withdraw_print = argv[++i];
            }
        } else if (w.rfind("--", 0) != 0 && a->package == nullptr && i == 3) {
            a->package = argv[i];
        } else {
            return false;
        }
    }
    const int forms = (a->give != nullptr) + a->list + (a->withdraw != nullptr);
    return forms == 1 && (a->give != nullptr) == (a->package != nullptr);
}

} // namespace

// plugins terms <pkg> [--json]
int plugins_terms(int argc, char** argv)
{
    const bool json = argc == 5 && std::string(argv[4]) == "--json";
    if (argc != 4 && !json) {
        std::fprintf(stderr, "havne-cli: plugins terms takes <pkg> [--json]\n");
        usage(stderr);
        return kExitUsage;
    }
    return run_plugins_terms(argv[3], json);
}

// plugins consent <pkg> --give <fingerprint>, --list or --withdraw <id>
// [<fingerprint>], each with [--consent-store <path>]
int plugins_consent(int argc, char** argv)
{
    ConsentArgs a;
    if (!parse_consent(argc, argv, &a)) {
        std::fprintf(stderr,
                     "havne-cli: plugins consent takes <pkg> --give "
                     "<fingerprint>, --list, or --withdraw <id> "
                     "[<fingerprint>], each with [--consent-store <path>]\n");
        usage(stderr);
        return kExitUsage;
    }
    if (a.give != nullptr) {
        return run_plugins_give(a.package, a.give, a.store);
    }
    return a.list ? run_plugins_list_consents(a.store)
                  : run_plugins_withdraw(a.withdraw, a.withdraw_print, a.store);
}

} // namespace cmd
