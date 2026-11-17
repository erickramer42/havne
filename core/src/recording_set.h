// File/layer: core/src/recording_set.h | internal, C++17, Qt-free; a
//             recording's header lines and the set that decodes it
//             (ADR-0054 section 1)
// Testing: UNIT: ExportTest, CsvExportTest, FolderExportTest, HealthTest
//          (test_core)
//
// What an export and the health report read of a recording before its
// rows: events.log's header (its time, profile, URI and the signals it
// declares text) and the set its values come from, by one rule: the
// recording's own signals.json, else uds-generic's set from the URI's
// did= list, else the profile by name.

#ifndef ECUIO_RECORDING_SET_H
#define ECUIO_RECORDING_SET_H

#include "error_detail.h"
#include "folder_files.h"
#include "frames_csv.h" /* read_line */
#include "recording_keys.h"
#include "recording_rows.h"
#include "signal_keys.h" /* the find rule, for a named column */
#include "uds_generic.h"
#include "uri_query.h"

#include "ecuio/profile.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ecuio_recording_set {

/* events.log's header: a recording without one gives the defaults */
struct header {
    uint64_t utc_us = 0; /* the recording's start; 0 none */
    std::string profile;
    std::string uri;
    /* Format 4: rows, labels and picks name a signal by its key */
    bool keyed = false;
    /* Signals declared text, each as signals.csv names it (ids' form) */
    std::vector<std::string> text_names;
    /* Every signal line as written, in its order (format 2 and later;
     * none in an older recording) */
    std::vector<ecuio_recording_rows::header_signal> signals;
    /* What signals.csv's signal cell holds for each of signals: its key
     * when keyed, else its name */
    std::vector<std::string> ids;
    /* The focus line's names in their order (ADR-0056 section 3); none
     * without one */
    std::vector<std::string> focus;
    /* Keyed: each signal line's key and name, sorted by key once read
     * (a vector: a map's move may throw) */
    std::vector<std::pair<std::string, std::string>> names;

    /* A signal's name for what signals.csv names it by: its signal line's
     * for a key (the first line's, as the reader keeps it), else the text
     * itself (a name, or a key no line declares) */
    const std::string& shown(const std::string& id) const
    {
        const auto it =
            std::lower_bound(names.begin(), names.end(), id,
                             [](const std::pair<std::string, std::string>& a,
                                const std::string& b) { return a.first < b; });
        return it == names.end() || it->first != id ? id : it->second;
    }
};

namespace detail {

struct file_closer {
    void operator()(FILE* f) const
    {
        if (f != nullptr) {
            (void)std::fclose(f);
        }
    }
};
using file_ptr = std::unique_ptr<FILE, file_closer>;

struct signal_file_free {
    void operator()(ecuio_signal_file_t* f) const { ecuio_signal_file_free(f); }
};

} // namespace detail

/* events.log's header lines, to its first blank line */
inline header read_header(const std::string& folder)
{
    namespace rows = ecuio_recording_rows;
    header h;
    std::string why;
    detail::file_ptr f(
        ecuio_files::open_file(folder + "/events.log", false, &why));
    if (!f) {
        return h;
    }
    std::vector<char> buf(rows::kRowMax + 2);
    bool too_long = false;
    bool first = true;
    while (ecuio_frames_csv::read_line(f.get(), buf, &too_long)) {
        if (too_long) {
            /* A line past a row (an older core's focus line): passed by
             * whole, so the signal lines after it are still read */
            bool more = true;
            while (more && ecuio_frames_csv::read_line(f.get(), buf, &more)) {
            }
            continue;
        }
        const size_t n = std::strlen(buf.data());
        if (first) {
            first = false;
            const size_t bom = rows::bom_length(buf.data(), n);
            h.keyed =
                ecuio_recording_keys::format_of(buf.data() + bom, n - bom) ==
                ECUIO_RECORDING_FORMAT_KEYS;
        }
        if (n == 0) {
            break;
        }
        if (rows::starts_with(buf.data(), n, "utc  ")) {
            (void)rows::parse_utc(buf.data() + 5, n - 5, &h.utc_us);
        } else if (rows::starts_with(buf.data(), n, "profile  ")) {
            h.profile.assign(buf.data() + 9, n - 9);
        } else if (rows::starts_with(buf.data(), n, "uri  ")) {
            h.uri.assign(buf.data() + 5, n - 5);
        } else if (rows::starts_with(buf.data(), n, "signal  ")) {
            rows::header_signal hs;
            const char* bad = nullptr;
            std::string id;
            if (ecuio_recording_keys::parse_signal_line(
                    h.keyed ? ECUIO_RECORDING_FORMAT_KEYS
                            : ECUIO_RECORDING_FORMAT_ENUMS,
                    buf.data() + 8, n - 8, &id, hs, &bad)) {
                if (hs.text) {
                    h.text_names.emplace_back(id);
                }
                if (h.keyed) {
                    h.names.emplace_back(id, hs.name);
                }
                h.signals.push_back(hs);
                h.ids.push_back(id);
            }
        } else if (rows::starts_with(buf.data(), n, "focus  ")) {
            const char* p = buf.data() + 7;
            const char* end = buf.data() + n;
            std::string name;
            bool last = false;
            while (!last && rows::next_field(p, end, name, &last)) {
                h.focus.push_back(name);
            }
        }
    }
    std::stable_sort(h.names.begin(), h.names.end(),
                     [](const std::pair<std::string, std::string>& a,
                        const std::pair<std::string, std::string>& b) {
                         return a.first < b.first;
                     });
    return h;
}

/* The set a recording's values were decoded by, held while it's used */
class set_of
{
public:
    /* The recording's own signals.json, else uds-generic's set from the
     * URI's did= list, else the profile by name; NULL for none (a raw
     * recording, or a profile this core doesn't have). A signals.json
     * there that doesn't load, or is past ECUIO_SIGNAL_SET_MAX_BYTES,
     * sets own_refused, with its words. */
    const ecuio_signal_set_t* load(const std::string& folder, const header& h)
    {
        set_ = nullptr;
        own_refused_ = false;
        own_why_.clear();
        std::string why;
        const std::string own = folder + "/signals.json";
        detail::file_ptr f(ecuio_files::open_file(own, false, &why));
        if (!f && ecuio_files::exists(own)) {
            own_refused_ = true;
            own_why_ = why;
        }
        if (f) {
            /* A byte past the bound: a longer file is refused whole, never
             * loaded from its first part. The copy is a session's whole
             * set, so the bound is a set's. */
            std::string bytes(ECUIO_SIGNAL_SET_MAX_BYTES + 1, '\0');
            bytes.resize(std::fread(&bytes[0], 1, bytes.size(), f.get()));
            ecuio_signal_file_t* loaded = nullptr;
            if (bytes.size() > ECUIO_SIGNAL_SET_MAX_BYTES) {
                own_refused_ = true;
                own_why_ = "it is past a recording's limit of " +
                           std::to_string(ECUIO_SIGNAL_SET_MAX_BYTES) +
                           " bytes for it";
            } else if (ecuio_signal_file_load_with(bytes.data(), bytes.size(),
                                                   ECUIO_SIGNAL_FILE_LOAD_SET,
                                                   &loaded) == ECUIO_OK) {
                file_.reset(loaded);
                set_ = ecuio_signal_file_set(loaded);
            } else {
                own_refused_ = true;
                own_why_ = ecuio_last_error_detail();
            }
        }
        if (set_ == nullptr && h.profile == ECUIO_UDS_GENERIC_NAME) {
            const size_t scheme = h.uri.find("://");
            ecuio_uri_query query;
            if (scheme != std::string::npos &&
                ecuio_uri::split(
                    ecuio_uri::query_of(h.uri.c_str() + scheme + 3), &query) ==
                    ecuio_uri::split_result::ok) {
                uds_.reset(new ecuio_uds_session);
                if (ecuio_uds_build(query, uds_.get()) == ECUIO_OK) {
                    set_ = &uds_->set;
                }
            }
        } else if (set_ == nullptr && !h.profile.empty()) {
            (void)ecuio_profile_lookup(h.profile.c_str(), &set_);
        }
        ecuio_detail_clear();
        return set_;
    }

    const ecuio_signal_set_t* set() const { return set_; }
    /* The recording's own signals.json when it gave the set, else NULL */
    const ecuio_signal_file_t* file() const { return file_.get(); }
    /* load found a signals.json it couldn't read or load */
    bool own_refused() const { return own_refused_; }
    const std::string& own_why() const { return own_why_; }

private:
    std::unique_ptr<ecuio_signal_file_t, detail::signal_file_free> file_;
    std::unique_ptr<ecuio_uds_session> uds_;
    const ecuio_signal_set_t* set_ = nullptr;
    bool own_refused_ = false;
    std::string own_why_;
};

/* A column a caller named, as signals.csv names it: by the find rule
 * over the header's signals (a key, or a name one signal has). A text no
 * signal has stays as given, an empty column. False for a name several
 * have, the detail the find's words. */
inline bool column_id(const header& h, const char* text, std::string* id)
{
    uint32_t index = 0;
    const ecuio_status_t found = ecuio_keys::find(
        static_cast<uint32_t>(h.signals.size()),
        [&h](uint32_t i) {
            return h.keyed ? std::string_view(h.ids[i]) : std::string_view();
        },
        [&h](uint32_t i) { return std::string_view(h.signals[i].name); }, text,
        &index);
    if (found == ECUIO_OK) {
        *id = h.ids[index];
        return true;
    }
    size_t named = 0;
    for (const ecuio_recording_rows::header_signal& hs : h.signals) {
        named += hs.name == text ? 1u : 0u;
    }
    if (named > 1) {
        return false;
    }
    ecuio_detail_clear();
    *id = text;
    return true;
}

/* What signals.csv's signal cell holds for each def of set, in the set's
 * order: its key in a recording kept by key (file the set's signal file
 * or NULL), its name before. Throws std::bad_alloc. */
inline std::vector<std::string> def_ids(const ecuio_signal_set_t* set,
                                        const ecuio_signal_file_t* file,
                                        bool keyed)
{
    std::vector<std::string> ids;
    if (set == nullptr || set->defs == nullptr) {
        return ids;
    }
    std::vector<ecuio_signal_key_t> keys;
    if (keyed && ecuio_recording_keys::set_keys(set, file, &keys) != ECUIO_OK) {
        throw std::bad_alloc();
    }
    ids.reserve(set->count);
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        ids.emplace_back(
            keyed ? std::string(keys[i].text)
                  : std::string(d.name, strnlen(d.name, sizeof d.name)));
    }
    return ids;
}

} // namespace ecuio_recording_set

#endif /* ECUIO_RECORDING_SET_H */
