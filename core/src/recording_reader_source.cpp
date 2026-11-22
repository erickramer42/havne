// File/layer: core/src/recording_reader_source.cpp | C++17, Qt-free;
//             the source's pass, signals.csv or frames.csv (ADR-0049
//             section 5 and its appendix of 2026-11-06)
// Testing: UNIT: RecordingTest, RecordingFramesTest, RecordingKeysTest
//          (test_core)

#include "recording_reader.h"
#include "error_detail.h"

#include <algorithm>

using ecuio_frames_csv::file_size;

using namespace ecuio_recording_reader;

/* The signal whose rows are under id, added with that name when new: a
 * row with no signal line above it shows its id as its name */
uint32_t ecuio_recording::add_signal(const std::string& id,
                                     const std::string& name)
{
    auto it = by_name.find(id);
    if (it != by_name.end()) {
        return it->second;
    }
    signal_entry s;
    std::memcpy(s.pub.name, name.data(), name.size());
    if (info.format == ECUIO_RECORDING_FORMAT_KEYS) {
        std::memcpy(s.pub.key, id.data(), id.size());
    }
    s.pub.first_us = ECUIO_RECORDING_NONE;
    s.pub.last_us = ECUIO_RECORDING_NONE;
    signals.push_back(std::move(s));
    const auto i = static_cast<uint32_t>(signals.size() - 1);
    by_name.emplace(id, i);
    return i;
}

bool ecuio_recording::read_signals(ecuio_open_progress_t progress, void* user,
                                   bool* canceled)
{
    file_ptr f = open_file(source_path);
    if (!f) {
        ecuio_detail_set("the folder has no signals.csv");
        return false;
    }
    const uint64_t total = file_size(f.get());
    if (progress != nullptr && progress(0, total, user) != 0) {
        *canceled = true;
        return false;
    }
    line_reader lines(f.get(), 0);
    std::string line;
    bool ended = false;
    bool too_long = false;
    if (!lines.next(line, &ended, &too_long) || !ended || too_long) {
        ecuio_detail_set("signals.csv has no header row");
        return false;
    }
    const size_t bom = rows::bom_length(line.data(), line.size());
    if (!rows::equals(line.data() + bom, line.size() - bom,
                      rows::kSignalsHeader)) {
        ecuio_detail_set("signals.csv's header row isn't %s",
                         rows::kSignalsHeader);
        return false;
    }
    end_offset = lines.offset();
    uint64_t line_no = 1;
    uint64_t prev_us = 0;
    uint64_t next_progress = kProgressBytes;
    rows::signal_row r;
    while (lines.next(line, &ended, &too_long)) {
        ++line_no;
        if (!ended) {
            ++info.rows_torn; /* section 7: a write cut short */
            break;
        }
        const char* why = nullptr;
        if (too_long) {
            why = "the row is longer than a recording's rows can be";
        } else if (!rows::parse_signal_row(line.data(), line.size(), r, &why)) {
            /* why is set */
        } else if (r.host_us < prev_us) {
            why = "host_us is earlier than the row before it";
        } else if (r.host_us / kSecondUs > UINT32_MAX) {
            why = "host_us is past what a recording spans";
        }
        signal_entry* s = nullptr;
        double v = 0.0;
        bool number = false;
        if (why == nullptr) {
            s = &signals[add_signal(r.name, r.name)];
            if (!s->known_kind && s->pub.unit[0] == '\0') {
                std::memcpy(s->pub.unit, r.unit.data(), r.unit.size());
            }
            number = rows::parse_number(r.value.data(), r.value.size(), &v);
            if (s->known_kind && !s->text() && !number) {
                why = "the value isn't a number";
            }
        }
        if (why != nullptr) {
            ecuio_detail_set("signals.csv line %llu: %s",
                             static_cast<unsigned long long>(line_no), why);
            return false;
        }
        prev_us = r.host_us;
        const auto second = static_cast<uint32_t>(r.host_us / kSecondUs);
        const uint64_t at = lines.line_start();
        if (index.empty() || index.back().second != second) {
            index.push_back(index_entry{at, second, 0});
        }
        end_offset = lines.offset();

        ecuio_recording_signal_t& pub = s->pub;
        ++pub.samples;
        if (pub.first_us == ECUIO_RECORDING_NONE) {
            pub.first_us = r.host_us;
        }
        pub.last_us = r.host_us;
        const bool track_text = !s->known_kind || s->text();
        const bool track_number = s->known_kind ? !s->text() : !s->not_number;
        if (!s->known_kind) {
            if (!number) {
                s->not_number = true;
            } else {
                s->decimals_seen =
                    (std::max)(s->decimals_seen,
                               rows::decimals_shown(r.value.data(),
                                                    r.value.size()));
            }
        }
        if (track_text) {
            if (s->texts.empty() || s->texts.back().second != second) {
                s->texts.push_back(text_second{second, 1, at});
            } else {
                ++s->texts.back().count;
                s->texts.back().last_offset = at;
            }
        }
        if (track_number && number) {
            if (s->numbers.empty() || s->numbers.back().second != second) {
                s->numbers.push_back(
                    number_second{second, 0, NAN, NAN, v, 0.0});
            }
            number_second& ns = s->numbers.back();
            ++ns.count;
            ns.last = v;
            ns.sum += v;
            merge(ns.lowest, ns.highest, v);
            merge(s->lowest, s->highest, v);
        }
        if (progress != nullptr && lines.offset() >= next_progress) {
            next_progress = lines.offset() + kProgressBytes;
            if (progress(lines.offset(), total, user) != 0) {
                *canceled = true;
                return false;
            }
        }
    }
    info.duration_us = (std::max)(info.duration_us, prev_us);
    if (progress != nullptr && progress(total, total, user) != 0) {
        *canceled = true;
        return false;
    }
    return true;
}

/* The set's defs as the signals, in its order, before frames.csv's pass:
 * each def's signal is its index until finish drops those with no sample */
void ecuio_recording::take_set(const ecuio_signal_set_t& set,
                               const std::vector<ecuio_signal_key_t>& set_keys)
{
    frame_set.reset(new set_copy(set));
    const ecuio_signal_set_t& s = frame_set->set;
    for (uint32_t i = 0; i < s.count; ++i) {
        const ecuio_signal_def_t& d = s.defs[i];
        signal_entry e;
        std::memcpy(e.pub.name, d.name, strnlen(d.name, sizeof d.name));
        std::memcpy(e.pub.unit, d.unit, strnlen(d.unit, sizeof d.unit));
        if (i < set_keys.size()) {
            std::memcpy(e.pub.key, set_keys[i].text, sizeof e.pub.key);
        }
        e.pub.first_us = ECUIO_RECORDING_NONE;
        e.pub.last_us = ECUIO_RECORDING_NONE;
        e.known_kind = true;
        const bool text =
            d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX;
        e.pub.flags = text                         ? ECUIO_RECORDING_SIGNAL_TEXT
                      : d.kind == ECUIO_VALUE_ENUM ? ECUIO_RECORDING_SIGNAL_ENUM
                                                   : 0;
        e.pub.decimal_places = d.decimal_places;
        e.pub.plot_rank = d.plot_rank;
        e.pub.group = d.group;
        if (d.kind == ECUIO_VALUE_ENUM && s.enums != nullptr) {
            e.labels.assign(s.enums + d.table_first,
                            s.enums + d.table_first + d.table_count);
        }
        signals.push_back(std::move(e));
        def_signal.push_back(i);
        def_of_id.emplace(d.id, i);
    }
}

/* The decoder's state before a second's first row, and rescan: the row a
 * re-read of the second starts from */
frame_mark ecuio_recording::mark(const frame_scan& s, uint64_t rescan)
{
    frame_mark m{rescan, kNoMark, kNoMark};
    const ecuio_frame_decoder& d = s.decoder;
    if (d.record_frame) {
        static const uint8_t kNone[256] = {};
        const uint8_t* last = lengths.empty() ? kNone : lengths.back().data();
        if (std::memcmp(d.learned_lengths, last, sizeof kNone) != 0) {
            lengths.emplace_back();
            std::memcpy(lengths.back().data(), d.learned_lengths, sizeof kNone);
        }
        if (!lengths.empty()) {
            m.lengths = static_cast<uint32_t>(lengths.size() - 1);
        }
    }
    if (!inputs.empty()) {
        m.latest = static_cast<uint32_t>(latest_pool.size());
        for (const uint32_t j : inputs) {
            const ecuio_frame_decoder::latest_value& l = d.latest[j];
            latest_pool.push_back(saved_latest{
                l.value, l.time_us,
                l.decode == 0 ? UINT64_MAX : d.decodes - l.decode});
        }
    }
    return m;
}

void ecuio_recording::restore(frame_scan& s, const frame_mark& m) const
{
    ecuio_frame_decoder& d = s.decoder;
    if (m.lengths != kNoMark) {
        std::memcpy(d.learned_lengths, lengths[m.lengths].data(),
                    sizeof d.learned_lengths);
    }
    if (m.latest != kNoMark) {
        d.decodes = kDecodeBase;
        for (size_t k = 0; k < inputs.size(); ++k) {
            const saved_latest& v = latest_pool[m.latest + k];
            d.latest[inputs[k]] = {v.value, v.time_us,
                                   v.age == UINT64_MAX ? 0
                                                       : kDecodeBase - v.age};
        }
    }
}

/* frames.csv in signals.csv's place: each row decoded as its session
 * decoded it, each value to signals.csv's text and back, so a number is
 * the double signals.csv would give */
bool ecuio_recording::read_frames(ecuio_open_progress_t progress, void* user,
                                  bool* canceled)
{
    file_ptr f = open_file(source_path);
    if (!f) {
        ecuio_detail_set("the folder has no frames.csv");
        return false;
    }
    const uint64_t total = file_size(f.get());
    if (progress != nullptr && progress(0, total, user) != 0) {
        *canceled = true;
        return false;
    }
    line_reader lines(f.get(), 0);
    std::string line;
    bool ended = false;
    bool too_long = false;
    if (!lines.next(line, &ended, &too_long) || !ended || too_long) {
        ecuio_detail_set("frames.csv has no header row");
        return false;
    }
    const size_t bom = rows::bom_length(line.data(), line.size());
    if (!rows::equals(line.data() + bom, line.size() - bom,
                      ecuio_frames_csv::kHeader)) {
        ecuio_detail_set("frames.csv's header row isn't %s",
                         ecuio_frames_csv::kHeader);
        return false;
    }
    end_offset = lines.offset();
    frame_scan scan(&frame_set->set);
    /* The number defs the computed signals read: a chain's computed
     * inputs keep no state, so a mark saves its leaves alone (ADR-0062) */
    for (const ecuio_frame_decoder::computed_plan& c : scan.decoder.computed) {
        for (uint32_t k = 0; k < c.program->input_count; ++k) {
            if (c.plan_of[k] == ecuio_frame_decoder::computed_plan::kLeaf &&
                std::find(inputs.begin(), inputs.end(), c.inputs[k]) ==
                    inputs.end()) {
                inputs.push_back(c.inputs[k]);
            }
        }
    }
    uint64_t rescan = lines.offset();
    uint64_t line_no = 1;
    uint64_t prev_us = 0;
    uint64_t next_progress = kProgressBytes;
    ecuio_frames_csv::row r;
    std::string value;
    while (lines.next(line, &ended, &too_long)) {
        ++line_no;
        if (!ended) {
            ++info.rows_torn; /* section 7: a write cut short */
            break;
        }
        const char* why = nullptr;
        if (too_long) {
            why = "the row is longer than a recording's rows can be";
        } else if (!ecuio_frames_csv::parse_row(line.c_str(), &r, &scan.row(),
                                                &why)) {
            /* why is set */
        } else if (r.host_us < prev_us) {
            why = "host_us is earlier than the row before it";
        } else if (r.host_us / kSecondUs > UINT32_MAX) {
            why = "host_us is past what a recording spans";
        }
        if (why != nullptr) {
            ecuio_detail_set("frames.csv line %llu: %s",
                             static_cast<unsigned long long>(line_no), why);
            return false;
        }
        prev_us = r.host_us;
        const auto second = static_cast<uint32_t>(r.host_us / kSecondUs);
        const uint64_t at = lines.line_start();
        if (index.empty() || index.back().second != second) {
            index.push_back(index_entry{at, second, 0});
            marks.push_back(mark(scan, rescan));
        }
        end_offset = lines.offset();
        const ecuio_frame_t* msg = scan.take(r.host_us);
        const uint32_t n = msg != nullptr ? scan.decoder.decode(msg) : 0;
        for (uint32_t k = 0; k < n; ++k) {
            const ecuio_value_t& v = scan.decoder.values[k];
            signal_entry& s = signals[def_of_id.at(v.signal_id)];
            double x = 0.0;
            if (!s.text()) {
                value.clear();
                rows::append_value(value, v.value);
                if (!rows::parse_number(value.data(), value.size(), &x) ||
                    !std::isfinite(x)) {
                    continue; /* not finite: no number to plot */
                }
            }
            ecuio_recording_signal_t& pub = s.pub;
            ++pub.samples;
            if (pub.first_us == ECUIO_RECORDING_NONE) {
                pub.first_us = r.host_us;
            }
            pub.last_us = r.host_us;
            if (s.text()) {
                if (s.texts.empty() || s.texts.back().second != second) {
                    /* no row to read back: text_at decodes its second */
                    s.texts.push_back(
                        text_second{second, 1, ECUIO_RECORDING_NONE});
                } else {
                    ++s.texts.back().count;
                }
                continue;
            }
            if (s.numbers.empty() || s.numbers.back().second != second) {
                s.numbers.push_back(number_second{second, 0, NAN, NAN, x, 0.0});
            }
            number_second& ns = s.numbers.back();
            ++ns.count;
            ns.last = x;
            ns.sum += x;
            merge(ns.lowest, ns.highest, x);
            merge(s.lowest, s.highest, x);
        }
        if (scan.idle()) {
            rescan = lines.offset();
        }
        if (progress != nullptr && lines.offset() >= next_progress) {
            next_progress = lines.offset() + kProgressBytes;
            if (progress(lines.offset(), total, user) != 0) {
                *canceled = true;
                return false;
            }
        }
    }
    info.duration_us = (std::max)(info.duration_us, prev_us);
    if (progress != nullptr && progress(total, total, user) != 0) {
        *canceled = true;
        return false;
    }
    return true;
}
