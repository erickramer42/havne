// File/layer: cli/settings_home.h | havne-cli, Qt-free, header-only
// Testing: HARNESS: CliConsentTest (tests/cli_consent.py: the default
//          under HAVNE_SETTINGS_DIR)
//
// The folder the app keeps its stores in, as QStandardPaths'
// AppDataLocation gives it for the organization and application "Havne"
// (app/main.cpp), so havne-cli's --consent-store defaults to the app's own
// consent.json (ADR-0066's appendix of 2026-11-23). HAVNE_SETTINGS_DIR
// first, as the app takes it; then %APPDATA%/Havne/Havne on Windows,
// ~/Library/Application Support/Havne/Havne on macOS, and
// $XDG_DATA_HOME/Havne/Havne (~/.local/share when unset) elsewhere.
// UTF-8, forward slashes; "" when no folder is known.

#ifndef HAVNE_CLI_SETTINGS_HOME_H
#define HAVNE_CLI_SETTINGS_HOME_H

#include <cstdlib>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cli {

// An environment variable's value in UTF-8, "" for none
inline std::string env_utf8(const char* name)
{
#ifdef _WIN32
    wchar_t wname[64] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64) == 0) {
        return "";
    }
    const DWORD n = GetEnvironmentVariableW(wname, nullptr, 0);
    if (n == 0) {
        return "";
    }
    std::wstring w(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(wname, &w[0], n);
    if (got == 0 || got >= n) {
        return "";
    }
    w.resize(got);
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                            nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(bytes), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                            &out[0], bytes, nullptr, nullptr);
    }
    return out;
#else
    const char* v = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    return v != nullptr ? v : "";
#endif
}

inline std::string settings_home()
{
    std::string dir = env_utf8("HAVNE_SETTINGS_DIR");
    if (dir.empty()) {
#if defined(_WIN32)
        const std::string base = env_utf8("APPDATA");
#elif defined(__APPLE__)
        const std::string home = env_utf8("HOME");
        const std::string base =
            home.empty() ? "" : home + "/Library/Application Support";
#else
        std::string base = env_utf8("XDG_DATA_HOME");
        if (base.empty() || base[0] != '/') {
            const std::string home = env_utf8("HOME");
            base = home.empty() ? "" : home + "/.local/share";
        }
#endif
        dir = base.empty() ? "" : base + "/Havne/Havne";
    }
#ifdef _WIN32
    for (char& c : dir) {
        if (c == '\\') {
            c = '/';
        }
    }
#endif
    return dir;
}

// A file in that folder, "" when no folder is known
inline std::string settings_file(const char* name)
{
    const std::string dir = settings_home();
    return dir.empty() ? "" : dir + "/" + name;
}

} // namespace cli

#endif // HAVNE_CLI_SETTINGS_HOME_H
