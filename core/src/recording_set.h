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
#include "recording_rows.h"
#include "uds_generic.h"
#include "uri_query.h"

#include "ecuio/profile.h"
#include "ecuio/signals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace ecuio_recording_set {

/* events.log's header: a recording without one gives the defaults */
struct header {
    uint64_t utc_us = 0; /* the recording's start; 0 none */
    std::string profile;
    std::string uri;
    std::vector<std::string> text_names; /* signals declared text */
    /* Every signal line as written, in its order (format 2 and later;
     * none in an older recording) */
    std::vector<ecuio_recording_rows::header_signal> signals;
    /* The focus line's names in their order (ADR-0056 section 3); none
     * without one */
    std::vector<std::string> focus;
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
    while (ecuio_frames_csv::read_line(f.get(), buf, &too_long)) {
        const size_t n = std::strlen(buf.data());
        if (too_long || n == 0) {
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
            if (rows::parse_header_signal(buf.data() + 8, n - 8, hs, &bad)) {
                if (hs.text) {
                    h.text_names.emplace_back(hs.name);
                }
                h.signals.push_back(hs);
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
    return h;
}

/* The set a recording's values were decoded by, held while it's used */
class set_of
{
public:
    /* The recording's own signals.json, else uds-generic's set from the
     * URI's did= list, else the profile by name; NULL for none (a raw
     * recording, or a profile this core doesn't have). A signals.json
     * there that doesn't load sets own_refused, with its words. */
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
            std::string bytes(ECUIO_SIGNAL_FILE_MAX_BYTES, '\0');
            bytes.resize(std::fread(&bytes[0], 1, bytes.size(), f.get()));
            ecuio_signal_file_t* loaded = nullptr;
            if (ecuio_signal_file_load(bytes.data(), bytes.size(), &loaded) ==
                ECUIO_OK) {
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

} // namespace ecuio_recording_set

#endif /* ECUIO_RECORDING_SET_H */
