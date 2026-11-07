// File/layer: core/src/scrub_frames.cpp | C++17, Qt-free; frames in memory
//             scrubbed as an export scrubs a recording's (ADR-0054
//             section 1, its appendix "frames in memory")
// Testing: UNIT: ScrubFramesTest (test_core)
//
// ecuio_scrub_frames: the caller's frames asked for twice. The first pass
// reassembles the frames of ISO-TP messages as they crossed the bus
// (frame_reassembly.h, ecuio_export's own) and classifies each frame and
// message with scrub.h, as ecuio_export's plan does, collecting the
// identity values the left-out replies carry. The second checks each kept
// frame, and each whole message a kept frame carries, for those values
// and for VIN-shaped tokens. Nothing is re-segmented: the caller writes
// the frames as they are.

#include "error_detail.h"
#include "frame_reassembly.h"
#include "scrub.h"

#include "ecuio/core.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

namespace fr = ecuio_frame_reassembly;
namespace scrub = ecuio_scrub;

constexpr uint32_t kIdentification = ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION;
constexpr uint32_t kUnattributable = ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE;
constexpr uint32_t kTextValues = ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES;

/* The two passes; a false return has set the detail and status_ */
class scrubber
{
public:
    scrubber(size_t count, ecuio_scrub_frame_callback_t get, void* user,
             const ecuio_signal_set_t* set, uint8_t* keep,
             ecuio_export_report_t& r)
        : count_(count), get_(get), user_(user), keep_(keep), r_(r),
          frame_(new ecuio_frame_t{}), probe_(new ecuio_frame_t{})
    {
        if (set != nullptr) {
            for (uint32_t i = 0; i < set->count; ++i) {
                const ecuio_signal_def_t& d = set->defs[i];
                if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 &&
                    (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX)) {
                    text_.push_back(&d);
                }
            }
        }
    }

    ecuio_status_t status() const { return status_; }

    bool plan()
    {
        std::fill(keep_, keep_ + count_, uint8_t{1});
        fr::reassembly rs;
        fr::other_id_identification others;
        std::vector<fr::message_end> ends;
        std::vector<uint8_t> whole;
        for (size_t i = 0; i < count_; ++i) {
            if (!fetch(i)) {
                return false;
            }
            const ecuio_frame_t& f = *frame_;
            ends.clear();
            if (fr::reassembly::on_the_wire(f)) {
                (void)rs.take(fr::seg{i, f.timestamp_us}, f, ends);
                if (!judge(ends)) {
                    return false;
                }
                continue;
            }
            if ((f.flags & ECUIO_FRAME_FLAG_REASSEMBLED) != 0 &&
                scrub::identification(f.data, f.data_length)) {
                if (!add_identity(f.data, f.data_length)) {
                    return false;
                }
                left_out(i, kIdentification);
            } else if (fr::reassembly::unread_reply(f)) {
                left_out(i, kUnattributable);
            } else if (others.take(f, &whole)) {
                if (!whole.empty() &&
                    !add_identity(whole.data(), whole.size())) {
                    return false;
                }
                left_out(i, kIdentification);
            } else if (text_message(f.arbitration_id, f.flags, f.data,
                                    f.data_length)) {
                left_out(i, kTextValues);
            }
        }
        ends.clear();
        rs.finish(ends);
        if (!judge(ends)) {
            return false;
        }
        std::vector<std::vector<uint8_t>> short_replies;
        others.finish(short_replies);
        for (const std::vector<uint8_t>& m : short_replies) {
            if (!add_identity(m.data(), m.size())) {
                return false;
            }
        }
        return true;
    }

    bool check()
    {
        fr::reassembly rs;
        fr::any_id_messages any;
        std::vector<fr::message_end> ends;
        std::vector<fr::joined> joins;
        for (size_t i = 0; i < count_; ++i) {
            if (!fetch(i)) {
                return false;
            }
            const ecuio_frame_t& f = *frame_;
            if (fr::reassembly::on_the_wire(f)) {
                ends.clear();
                (void)rs.take(fr::seg{i, f.timestamp_us}, f, ends);
                if (!check_ends(ends)) {
                    return false;
                }
            } else if (fr::tp_readable(f)) {
                joins.clear();
                any.take(f, keep_[i] != 0, joins);
                if (!check_joined(joins)) {
                    return false;
                }
            }
            if (keep_[i] != 0 && !check_bytes(f.data, f.data_length)) {
                return false;
            }
        }
        ends.clear();
        rs.finish(ends);
        joins.clear();
        any.finish(joins);
        if (!check_ends(ends) || !check_joined(joins)) {
            return false;
        }
        for (size_t i = 0; i < count_; ++i) {
            r_.frames += keep_[i];
        }
        return true;
    }

private:
    bool fetch(size_t i)
    {
        std::memset(frame_.get(), 0, sizeof *frame_);
        if (get_(user_, i, frame_.get()) != 0) {
            status_ = ECUIO_ERR_CANCELED;
            ecuio_detail_set("the caller canceled");
            return false;
        }
        if (frame_->data_length > ECUIO_FRAME_DATA_MAX) {
            status_ = ECUIO_ERR_INVALID_ARGUMENT;
            ecuio_detail_set("frame %llu holds more bytes than a frame can",
                             static_cast<unsigned long long>(i));
            return false;
        }
        return true;
    }

    void left_out(size_t i, uint32_t cls)
    {
        if (keep_[i] != 0) {
            keep_[i] = 0;
            ++r_.left_out[cls];
        }
    }

    bool add_identity(const uint8_t* msg, size_t n)
    {
        std::vector<std::string> v;
        scrub::identification_values(msg, n, v);
        for (const std::string& s : v) {
            if (!values_.add(s.data(), s.size(), kIdentification)) {
                status_ = ECUIO_ERR_LOAD_FAILED;
                ecuio_detail_set("the frames hold more identity values than "
                                 "the check can take");
                return false;
            }
        }
        return true;
    }

    /* A message a text or hex def applies to */
    bool text_message(uint32_t id, uint32_t flags, const uint8_t* data,
                      size_t n)
    {
        if (text_.empty() || n > ECUIO_FRAME_DATA_MAX) {
            return false;
        }
        probe_->arbitration_id = id;
        probe_->flags = flags;
        probe_->data_length = static_cast<uint16_t>(n);
        std::memcpy(probe_->data, data, n);
        for (const ecuio_signal_def_t* d : text_) {
            if (ecuio_def_applies_to_frame(d, probe_.get()) != 0) {
                return true;
            }
        }
        return false;
    }

    /* Messages reassembled from the wire: an identification reply's
     * frames left out, a text or hex def's, a torn one's on a reply ID */
    bool judge(const std::vector<fr::message_end>& ends)
    {
        for (const fr::message_end& e : ends) {
            uint32_t cls = ECUIO_EXPORT_LEFT_OUT_COUNT;
            if (e.whole) {
                if (scrub::identification(e.data.data(), e.data.size())) {
                    if (!add_identity(e.data.data(), e.data.size())) {
                        return false;
                    }
                    cls = kIdentification;
                } else if (text_message(
                               e.id,
                               (e.extended ? ECUIO_FRAME_FLAG_EXTENDED : 0u) |
                                   ECUIO_FRAME_FLAG_REASSEMBLED,
                               e.data.data(), e.data.size())) {
                    cls = kTextValues;
                }
            } else if (scrub::segment_reply_id(e.id, e.extended)) {
                cls = kUnattributable;
            }
            if (cls != ECUIO_EXPORT_LEFT_OUT_COUNT) {
                for (const fr::seg& s : e.frames) {
                    left_out(static_cast<size_t>(s.index), cls);
                }
            }
        }
        return true;
    }

    bool check_bytes(const uint8_t* p, size_t n)
    {
        const uint32_t found = scrub::check_bytes(values_, p, n);
        if (found != 0) {
            r_.failed_class = found;
            status_ = ECUIO_ERR_LOAD_FAILED;
            ecuio_detail_set("the check found %s in a frame the export would "
                             "write: nothing was written",
                             scrub::failed_words(found));
            return false;
        }
        return true;
    }

    /* The messages any ISO-TP-shaped frames join that a kept frame
     * carries (any_id_messages) */
    bool check_joined(const std::vector<fr::joined>& joins)
    {
        for (const fr::joined& j : joins) {
            if (j.written && !check_bytes(j.data.data(), j.data.size())) {
                return false;
            }
        }
        return true;
    }

    /* The whole messages a kept frame carries */
    bool check_ends(const std::vector<fr::message_end>& ends)
    {
        for (const fr::message_end& e : ends) {
            if (!e.whole || e.frames.empty() ||
                keep_[static_cast<size_t>(e.frames[0].index)] == 0) {
                continue;
            }
            if (!check_bytes(e.data.data(), e.data.size())) {
                return false;
            }
        }
        return true;
    }

    const size_t count_;
    const ecuio_scrub_frame_callback_t get_;
    void* const user_;
    uint8_t* const keep_;
    ecuio_export_report_t& r_;
    std::unique_ptr<ecuio_frame_t> frame_;
    std::unique_ptr<ecuio_frame_t> probe_;
    std::vector<const ecuio_signal_def_t*> text_;
    scrub::values values_;
    ecuio_status_t status_ = ECUIO_OK;
};

} // namespace

extern "C" ecuio_status_t
ecuio_scrub_frames(size_t count, ecuio_scrub_frame_callback_t get,
                   void* user_data, const ecuio_signal_set_t* set,
                   uint32_t flags, uint8_t* keep, ecuio_export_report_t* report)
{
    if (report != nullptr) {
        *report = ecuio_export_report_t{};
    }
    if (get == nullptr || (keep == nullptr && count != 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (flags != 0) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_detail_clear();
    ecuio_export_report_t r{};
    ecuio_status_t rc = ECUIO_OK;
    try {
        scrubber s(count, get, user_data, set, keep, r);
        if (!s.plan() || !s.check()) {
            rc = s.status();
        }
    } catch (const std::bad_alloc&) {
        rc = ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (rc != ECUIO_OK && count != 0) {
        std::memset(keep, 0, count);
        r.frames = 0;
    }
    if (report != nullptr) {
        *report = r;
    }
    return rc;
}
