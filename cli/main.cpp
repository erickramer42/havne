// File/layer: cli/main.cpp | havne-cli, a command-line client over the C ABI (ADR-0031)
// Testing: UNIT: CliGoldenTest, CliUsageTest, CliConnectTest, CliSignalTest,
//          CliReplayTest, CliRecordTest, CliViewRunTest, CliPathTest,
//          CliTransferRunTest, CliExitTest, CliObdbImportTest, CliLogRunTest,
//          CliPluginMakeTest
//          (ctest);
//          CliTimingTest, CliVinTest, CliSimTest, CliMarkLinesTest,
//          CliViewTest, CliInfoTest, CliSignalFileTest, CliHealthTest,
//          CliPartialTest
//          (test_cli); BENCH
//
// Public headers only: what any program linking core can do.

#include "commands.h"
#include "common.h"

#include "ecuio/core.h"

using namespace cmd;

namespace {

// plugins list's packages, an option refused
int plugins_list(int argc, char** argv)
{
    std::vector<const char*> paths;
    for (int i = 3; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
            usage(stderr);
            return kExitUsage;
        }
        paths.push_back(argv[i]);
    }
    return run_plugins_list(paths);
}

// plugins check's package, and --on <vcan> read and checked
int plugins_check(int argc, char** argv)
{
    const char* on = nullptr;
    if (argc == 6) {
        if (std::string(argv[4]) != "--on" || *argv[5] == '\0') {
            std::fprintf(stderr, "havne-cli: plugins check takes "
                                 "<pkg> [--on <vcan>]\n");
            usage(stderr);
            return kExitUsage;
        }
        on = argv[5];
    }
    return run_plugins_check(argv[3], on);
}

// wrap, put and drop: their words through parse_plugin_make, then the run
int plugins_make(const std::string& word, int argc, char** argv)
{
    PluginMakeArgs make;
    if (word == "wrap") {
        make.file = argv[3];
    } else {
        make.package = argv[3];
        make.file = word == "put" ? argv[4] : nullptr;
    }
    std::string why;
    if (!parse_plugin_make(word, word == "put" ? 5 : 4, argc, argv, &make,
                           &why)) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        usage(stderr);
        return kExitUsage;
    }
    return word == "wrap"  ? run_plugins_wrap(make)
           : word == "put" ? run_plugins_put(make)
                           : run_plugins_drop(make);
}

// plugins' words, then each one's run
int cmd_plugins(int argc, char** argv)
{
    const std::string word = argc > 2 ? argv[2] : "";
    if (word == "list" && argc > 3) {
        return plugins_list(argc, argv);
    }
    const bool package = argc > 3 && std::string(argv[3]).rfind("--", 0) != 0;
    if (word == "check" && package && (argc == 4 || argc == 6)) {
        return plugins_check(argc, argv);
    }
    if (word == "match" && package && argc == 6 &&
        std::string(argv[4]) == "--vin") {
        return run_plugins_match(argv[3], argv[5]);
    }
    const bool two =
        package && argc > 4 && std::string(argv[4]).rfind("--", 0) != 0;
    if ((word == "wrap" && package) || (word == "put" && two) ||
        (word == "drop" && package)) {
        return plugins_make(word, argc, argv);
    }
    std::fprintf(stderr, "havne-cli: plugins takes list <pkg>..., check <pkg> "
                         "[--on <vcan>], match <pkg> --vin <VIN>, wrap "
                         "<signal-file> --out <pkg>, put <pkg> <signal-file> "
                         "--out <pkg>, or drop <pkg> --variant <id> --out "
                         "<pkg>\n");
    usage(stderr);
    return kExitUsage;
}

int run(int argc, char** argv)
{
#ifdef _WIN32
    // JSON lines end in \n on every platform
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) {
        usage(stderr);
        return kExitUsage;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        usage(stdout);
        return kExitOk;
    }
    if (command == "--version") {
        std::printf("havne-cli %s\n", ecuio_version_string());
        return kExitOk;
    }
    if (command == "devices" && argc == 2) {
        return run_devices();
    }
    if (command == "vin" && argc == 3) {
        return run_vin(argv[2]);
    }
    if (command == "dtc" && argc == 3) {
        return run_dtc(argv[2]);
    }
    if (command == "view") {
        return cmd_view(argc, argv);
    }
    if (command == "info") {
        return cmd_info(argc, argv);
    }
    if (command == "health") {
        return cmd_health(argc, argv);
    }
    if (command == "frames") {
        return cmd_frames(argc, argv);
    }
    if (command == "export") {
        return cmd_export(argc, argv);
    }
    if (command == "import") {
        return cmd_import(argc, argv);
    }
    if (command == "play") {
        return cmd_play(argc, argv);
    }
    if (command == "clear") {
        return cmd_clear(argc, argv);
    }
    if (command == "sim") {
        return cmd_sim(argc, argv);
    }
    if (command == "plugins") {
        return cmd_plugins(argc, argv);
    }
    if (command == "conditions") {
        return cmd_conditions(argc, argv);
    }
    if (command == "layout") {
        return cmd_layout(argc, argv);
    }
    if (command == "signals") {
        return cmd_signals(argc, argv);
    }
    if (command == "log") {
        return cmd_log(argc, argv);
    }
    if (command != "stream") {
        usage(stderr);
        return kExitUsage;
    }
    return cmd_stream(argc, argv);
}

} // namespace

#ifdef _WIN32
// Windows hands the arguments over as UTF-16 here; main's char** would be
// the ANSI code page's, where a path with a character outside it (an
// accented user name's folder in another code page, Greek, CJK) comes as
// '?'. Each is UTF-8 from here on, as core's paths are.
int wmain(int argc, wchar_t** wargv)
{
    try {
        std::vector<std::string> args(static_cast<size_t>(argc));
        std::vector<char*> argv(static_cast<size_t>(argc) + 1, nullptr);
        for (int i = 0; i < argc; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr,
                                              0, nullptr, nullptr);
            if (n > 0) {
                args[i].resize(static_cast<size_t>(n));
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &args[i][0], n,
                                    nullptr, nullptr);
                args[i].resize(static_cast<size_t>(n - 1));
            }
            argv[i] = &args[i][0];
        }
        return run(argc, argv.data());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "havne-cli: %s\n", e.what());
        return kExitFailed;
    }
}
#else
int main(int argc, char** argv)
{
    // What throws (an allocation, a thread that won't start) ends as the
    // ring's out of memory does: a CLI failure with its reason, not
    // std::terminate
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "havne-cli: %s\n", e.what());
        return kExitFailed;
    }
}
#endif
