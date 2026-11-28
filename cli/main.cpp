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
    if (command == "dump") {
        return cmd_dump(argc, argv);
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
