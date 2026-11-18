// File/layer: cli/signal_pairs.h | havne-cli and the app's viewer, a compared signal's pair (ADR-0065 section 9)
// Testing: UNIT: CliPairsTest (test_cli); BRIDGE: RecordingViewTest (test_bridge)
//
// Which signal of another recording a signal compares with, the one rule
// for view's delta and the app's Compare: the signal of its key, when it
// has one and the other recording holds it; else the signal of its name,
// when that name is one signal's in each recording. A name two signals of
// either recording have pairs nothing by name, since nothing says which
// of them the other recorded; a signal the other recording's key pairs
// is never a name's. An "id:<n>" key is a set's own numbering,
// so it pairs by name: two sets' "id:5" are two signals. A pair's unit
// is the caller's to compare (same_unit). Public headers only.

#ifndef HAVNE_CLI_SIGNAL_PAIRS_H
#define HAVNE_CLI_SIGNAL_PAIRS_H

#include "ecuio/recording.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cli {

// A signal with no pair
inline constexpr uint32_t kNoPair = UINT32_MAX;

// A signal's text field up to its NUL, or its whole size with none
template <size_t N> inline std::string field_text(const char (&f)[N])
{
    return std::string(f, strnlen(f, N));
}

// Whether a key goes across recordings: an address or an author's key,
// never "" (a recording older than format 4) or a set's "id:<n>"
inline bool key_pairs(const std::string& key)
{
    return !key.empty() && key.compare(0, 3, "id:") != 0;
}

// Whether two signals are in one unit, its ASCII case ignored ("RPM" is
// "rpm"), so a speed in km/h is never taken from one in mph
inline bool same_unit(const ecuio_recording_signal_t& a,
                      const ecuio_recording_signal_t& b)
{
    auto lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const std::string x = field_text(a.unit);
    const std::string y = field_text(b.unit);
    if (x.size() != y.size()) {
        return false;
    }
    for (size_t i = 0; i < x.size(); ++i) {
        if (lower(x[i]) != lower(y[i])) {
            return false;
        }
    }
    return true;
}

// For each of a's signals, b's signal it pairs with, or kNoPair. a_at(k)
// and b_at(k) give a recording's signal k as ecuio_recording_signal_t.
// May throw std::bad_alloc.
template <typename A, typename B>
std::vector<uint32_t> pair_signals(uint32_t a_count, const A& a_at,
                                   uint32_t b_count, const B& b_at)
{
    std::unordered_map<std::string, uint32_t> own;
    std::unordered_set<std::string> own_keys;
    for (uint32_t k = 0; k < a_count; ++k) {
        const ecuio_recording_signal_t& s = a_at(k);
        ++own[field_text(s.name)];
        std::string key = field_text(s.key);
        if (key_pairs(key)) {
            own_keys.insert(std::move(key));
        }
    }
    std::unordered_map<std::string, uint32_t> keys;
    // A name's signal, or kNoPair for a name two signals have or whose
    // signal pairs by key: one signal of b pairs with one of a at most
    std::unordered_map<std::string, uint32_t> names;
    for (uint32_t k = 0; k < b_count; ++k) {
        const ecuio_recording_signal_t& s = b_at(k);
        const std::string key = field_text(s.key);
        const bool claimed = key_pairs(key) && own_keys.count(key) != 0;
        if (key_pairs(key)) {
            keys.emplace(key, k);
        }
        const auto put =
            names.emplace(field_text(s.name), claimed ? kNoPair : k);
        if (!put.second) {
            put.first->second = kNoPair;
        }
    }
    std::vector<uint32_t> out(a_count, kNoPair);
    for (uint32_t k = 0; k < a_count; ++k) {
        const ecuio_recording_signal_t& s = a_at(k);
        const auto by_key = keys.find(field_text(s.key));
        if (by_key != keys.end()) {
            out[k] = by_key->second;
            continue;
        }
        const std::string name = field_text(s.name);
        const auto by_name = names.find(name);
        if (own[name] == 1 && by_name != names.end()) {
            out[k] = by_name->second;
        }
    }
    return out;
}

} // namespace cli

#endif
