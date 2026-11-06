// File/layer: cli/recording_signals.h | havne-cli and the app, a replay's own signal file (ADR-0051)
// Testing: UNIT: CliRecordingSignalsTest (test_cli); BRIDGE: BridgeTest.Replay*OwnSignalFile* (test_bridge)
//
// A recording made with a signal file holds a copy, signals.json, its
// SHA-256 on events.log's header (ADR-0051 section 4). A replay decodes
// with that copy when the URI selects the recorded profile (or, for
// havne-cli, none), so a recording whose file has since changed, been
// removed, or was made on another machine replays as it was recorded.
// This header holds the rules both front ends share: reading the header's
// two lines, the replay URI's folder, and the URI with another profile.
// Each front end reads the files and checks the hash its own way.

#ifndef HAVNE_CLI_RECORDING_SIGNALS_H
#define HAVNE_CLI_RECORDING_SIGNALS_H

#include <cstddef>
#include <string>

namespace cli {

// What events.log's header says decoded the session: its "profile" line
// and its "signal file  signals.json,<SHA-256>" line (format 3)
struct RecordedSignals {
    std::string profile; // "" when the header has none
    std::string sha256;  // 64 lowercase hex digits; "" when it names no file
    bool ended = false;  // the header's blank line was seen

    // Takes one line of events.log from its first, its line end cut;
    // false once the header has ended (the blank line), so a caller stops
    // reading there
    bool take(const std::string& line)
    {
        if (ended) {
            return false;
        }
        std::string l = line;
        if (!l.empty() && l.back() == '\r') {
            l.pop_back();
        }
        if (l.empty()) {
            ended = true;
            return false;
        }
        static const char kProfile[] = "profile  ";
        static const char kFile[] = "signal file  signals.json,";
        const size_t np = sizeof kProfile - 1;
        const size_t nf = sizeof kFile - 1;
        if (l.compare(0, np, kProfile) == 0) {
            profile = l.substr(np);
        } else if (l.compare(0, nf, kFile) == 0 && l.size() == nf + 64) {
            const std::string hex = l.substr(nf);
            bool ok = true;
            for (const char c : hex) {
                ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
            }
            if (ok) {
                sha256 = hex;
            }
        }
        return true;
    }

    // A file named and a profile it was recorded under
    bool names_file() const { return !sha256.empty() && !profile.empty(); }
};

// The profile the recording's copy registers under when the recorded
// name holds other bytes (the app's signal store with an edited file):
// "rec." and the hash's first 16 digits, a name the URI can select
inline std::string own_profile(const std::string& sha256)
{
    return "rec." + sha256.substr(0, 16);
}

// A replay:// URI's recording folder: the text between the scheme and
// the query, or the folder of a path ending ".csv" (core's rule: a path
// ending .csv is frames.csv itself). false for another scheme or no path
inline bool replay_folder(const std::string& uri, std::string* folder)
{
    static const char kScheme[] = "replay://";
    const size_t ns = sizeof kScheme - 1;
    if (uri.size() <= ns) {
        return false;
    }
    for (size_t i = 0; i < ns; ++i) {
        const char c = uri[i];
        const char lower =
            (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        if (lower != kScheme[i]) {
            return false;
        }
    }
    std::string path =
        uri.substr(ns, uri.find('?') == std::string::npos ? std::string::npos
                                                          : uri.find('?') - ns);
    if (path.size() > 4) {
        std::string tail = path.substr(path.size() - 4);
        for (char& c : tail) {
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }
        if (tail == ".csv") {
            const size_t slash = path.find_last_of("/\\");
            path = slash == std::string::npos ? std::string()
                                              : path.substr(0, slash);
        }
    }
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) {
        path.pop_back();
    }
    if (path.empty()) {
        return false;
    }
    *folder = path;
    return true;
}

// The URI with profile=<profile> as its one profile: every profile= in
// the query dropped and the new one added last, the other pairs kept in
// their order
inline std::string with_profile(const std::string& uri,
                                const std::string& profile)
{
    const size_t q = uri.find('?');
    std::string out = uri.substr(0, q);
    std::string query;
    if (q != std::string::npos) {
        size_t at = q + 1;
        while (at <= uri.size()) {
            size_t end = uri.find('&', at);
            if (end == std::string::npos) {
                end = uri.size();
            }
            const std::string pair = uri.substr(at, end - at);
            if (!pair.empty() && pair.compare(0, 8, "profile=") != 0) {
                query += (query.empty() ? "" : "&") + pair;
            }
            at = end + 1;
        }
    }
    query += (query.empty() ? "" : "&") + std::string("profile=") + profile;
    return out + "?" + query;
}

} // namespace cli

#endif // HAVNE_CLI_RECORDING_SIGNALS_H
