#ifndef ECUIO_OPERATION_FLASH_H
#define ECUIO_OPERATION_FLASH_H

#include "operation_plan.h"
#include "request_talk.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ecuio_operation {

constexpr uint32_t kFlashWireMost = ECUIO_ENGINE_WRITE_MAX;
constexpr uint32_t kFlashPauseMs = 2000;
constexpr uint32_t kFlashPausesMost = 30;

enum class flash_stage : uint8_t { erase, open, block, exit, check, done };

enum class flash_end : uint8_t {
    none,
    written,
    refused,
    no_answer,
    bad_reply,
    canceled,
    check_failed,
    file,      /* the journal couldn't be written before the floor */
    link_lost, /* a request couldn't be handed to the adapter */
};

enum class flash_action : uint8_t {
    send,
    journal, /* flush the journal, then journaled() */
    pause,   /* wait kFlashPauseMs, then next() */
    done,
};

enum class flash_wait : uint8_t { go_on, keep_waiting };

struct flash_sector {
    uint32_t step = 0;
    uint64_t address = 0;
    uint64_t bytes = 0;
    uint64_t offset = 0; /* into its step's image */
};

struct flash_run {
    const plan* p = nullptr;
    flash_stage at = flash_stage::erase;
    flash_end result = flash_end::none;
    uint8_t nrc = 0;
    bool floor = false;
    bool asked = false;
    bool canceled = false;
    bool open = false;
    bool journal_due = true;
    bool resent = false;
    bool paused = false;
    uint32_t pauses = 0;
    uint32_t wire_max = 0;
    uint32_t block = 0;
    uint8_t counter = 0;
    uint32_t chosen = 0;
    uint32_t sector = 0;
    uint64_t sent = 0;
    uint64_t written = 0;
    uint32_t done_sectors = 0;
    flash_sector sectors[ECUIO_PLUGIN_MAX_SECTORS] = {};
};

/* Where a sector lies among the plan's images, laid end to end */
inline bool flash_locate(const plan& p, const sector& c, flash_sector* out,
                         uint64_t* flat)
{
    uint64_t base = 0;
    for (uint32_t i = 0; i < p.steps.size(); ++i) {
        const step& s = p.steps[i];
        if (c.address >= s.address && c.address - s.address < s.bytes) {
            out->step = i;
            out->address = c.address;
            out->bytes = c.bytes;
            out->offset = c.address - s.address;
            *flat = base + out->offset;
            return true;
        }
        base += s.bytes;
    }
    return false;
}

/* The sectors to write: each whose image differs from current (the
 * images' bytes end to end, as a copy of them holds), every one with
 * no current, or with a dfi that isn't plain */
inline uint32_t flash_select(const plan& p, const uint8_t* current,
                             flash_sector* out)
{
    bool plain = current != nullptr;
    for (const step& s : p.steps) {
        plain = plain && s.dfi == 0;
    }
    uint32_t n = 0;
    for (const sector& c : p.writes.sectors) {
        flash_sector f;
        uint64_t flat = 0;
        if (!flash_locate(p, c, &f, &flat)) {
            continue;
        }
        const uint8_t* img = p.steps[f.step].blob.data() + f.offset;
        if (!plain || std::memcmp(img, current + flat,
                                  static_cast<size_t>(c.bytes)) != 0) {
            out[n++] = f;
        }
    }
    return n;
}

inline void flash_begin(flash_run& r, const plan& p, uint32_t wire_max,
                        const uint8_t* current)
{
    r = flash_run{};
    r.p = &p;
    r.wire_max = wire_max < kFlashWireMost ? wire_max : kFlashWireMost;
    r.chosen = flash_select(p, current, r.sectors);
    r.at = r.chosen == 0 ? flash_stage::check : flash_stage::erase;
}

inline uint64_t flash_bytes(const flash_run& r)
{
    uint64_t n = 0;
    for (uint32_t i = 0; i < r.chosen; ++i) {
        n += r.sectors[i].bytes;
    }
    return n;
}

inline void flash_stop(flash_run& r, flash_end e, uint8_t nrc = 0)
{
    r.at = flash_stage::done;
    r.result = e;
    r.nrc = nrc;
    r.asked = false;
}

inline uint32_t flash_block_bytes(const flash_run& r)
{
    const uint64_t left = r.sectors[r.sector].bytes - r.sent;
    return static_cast<uint32_t>(left < r.block ? left : r.block);
}

inline uint8_t flash_service(const flash_run& r)
{
    switch (r.at) {
    case flash_stage::open:
        return 0x34;
    case flash_stage::block:
        return 0x36;
    case flash_stage::exit:
        return 0x37;
    default:
        return 0x31;
    }
}

inline uint8_t* flash_put(uint8_t* out, uint64_t value, unsigned n)
{
    for (unsigned i = n < 4 ? n : 4; i > 0; --i) {
        *out++ = static_cast<uint8_t>(value >> (8 * (i - 1)));
    }
    return out;
}

/* out holds kFlashWireMost bytes */
inline uint32_t flash_request(const flash_run& r, uint8_t* out)
{
    uint8_t* at = out;
    *at++ = flash_service(r);
    if (r.at == flash_stage::block) {
        const flash_sector& c = r.sectors[r.sector];
        const uint32_t n = flash_block_bytes(r);
        *at++ = r.counter;
        std::memcpy(at, r.p->steps[c.step].blob.data() + c.offset + r.sent, n);
        return static_cast<uint32_t>(at - out) + n;
    }
    if (r.at == flash_stage::exit) {
        return 1;
    }
    const bool check = r.at == flash_stage::check;
    if (r.at != flash_stage::open) {
        const uint16_t id = check ? r.p->writes.check : r.p->writes.erase;
        *at++ = 0x01;
        *at++ = static_cast<uint8_t>(id >> 8);
        *at++ = static_cast<uint8_t>(id);
    } else {
        *at++ = r.p->steps[r.sectors[r.sector].step].dfi;
    }
    if (check) {
        return static_cast<uint32_t>(at - out);
    }
    const flash_sector& c = r.sectors[r.sector];
    const step& s = r.p->steps[c.step];
    *at++ = static_cast<uint8_t>((s.size_bytes << 4) | s.address_bytes);
    at = flash_put(at, c.address, s.address_bytes);
    at = flash_put(at, c.bytes, s.size_bytes);
    return static_cast<uint32_t>(at - out);
}

/* What to do now; call it only while no request is out */
inline flash_action flash_next(flash_run& r, uint8_t* out, uint32_t* len)
{
    *len = 0;
    if (r.at == flash_stage::done) {
        return flash_action::done;
    }
    if (r.journal_due && (r.at == flash_stage::erase || r.floor)) {
        return flash_action::journal;
    }
    if (r.paused) {
        r.paused = false;
        return flash_action::pause;
    }
    *len = flash_request(r, out);
    r.asked = true;
    return flash_action::send;
}

/* The journal holds what the run is about to do */
inline void flash_journaled(flash_run& r)
{
    r.journal_due = false;
}

/* A 74's maxNumberOfBlockLength counts the 36 and its counter */
inline bool flash_opened(flash_run& r, const uint8_t* d, size_t n)
{
    const size_t sized = n >= 2 ? d[1] >> 4 : 0;
    if (d[0] != 0x74 || sized < 1 || sized > 4 || n != 2 + sized) {
        return false;
    }
    uint64_t most = 0;
    for (size_t i = 0; i < sized; ++i) {
        most = most << 8 | d[2 + i];
    }
    if (most > r.wire_max) {
        most = r.wire_max;
    }
    if (most < 3) {
        return false;
    }
    r.block = static_cast<uint32_t>(most - 2);
    r.counter = 1;
    r.sent = 0;
    r.open = true;
    r.at = flash_stage::block;
    return true;
}

inline bool flash_routine_echo(const flash_run& r, const uint8_t* d, size_t n,
                               uint16_t id)
{
    return n >= 4 && d[0] == 0x71 && d[1] == 0x01 &&
           d[2] == static_cast<uint8_t>(id >> 8) &&
           d[3] == static_cast<uint8_t>(id) && r.p != nullptr;
}

inline void flash_checked(flash_run& r, const uint8_t* d, size_t n)
{
    const std::vector<uint8_t>& pass = r.p->writes.pass;
    if (!flash_routine_echo(r, d, n, r.p->writes.check)) {
        flash_stop(r, flash_end::bad_reply);
        return;
    }
    const bool passed = n >= 4 + pass.size() &&
                        std::memcmp(d + 4, pass.data(), pass.size()) == 0;
    flash_stop(r, passed ? flash_end::written : flash_end::check_failed);
}

inline void flash_block_taken(flash_run& r)
{
    const uint32_t n = flash_block_bytes(r);
    r.sent += n;
    r.written += n;
    ++r.counter;
    r.resent = false;
    r.pauses = 0;
    if (r.sent == r.sectors[r.sector].bytes) {
        r.at = flash_stage::exit;
    }
}

inline void flash_positive(flash_run& r, const uint8_t* d, size_t n)
{
    switch (r.at) {
    case flash_stage::erase:
        if (!flash_routine_echo(r, d, n, r.p->writes.erase)) {
            flash_stop(r, flash_end::bad_reply);
            return;
        }
        r.at = flash_stage::open;
        return;
    case flash_stage::open:
        if (!flash_opened(r, d, n)) {
            flash_stop(r, flash_end::bad_reply);
        }
        return;
    case flash_stage::block:
        if (n != 2 || d[0] != 0x76 || d[1] != r.counter) {
            flash_stop(r, flash_end::bad_reply);
            return;
        }
        flash_block_taken(r);
        return;
    case flash_stage::exit:
        if (d[0] != 0x77) {
            flash_stop(r, flash_end::bad_reply);
            return;
        }
        r.open = false;
        ++r.done_sectors;
        r.journal_due = true;
        r.at = ++r.sector < r.chosen ? flash_stage::erase : flash_stage::check;
        return;
    default:
        flash_checked(r, d, n);
        return;
    }
}

/* A refused 36: 73 sends the block once more, 92 and 93 wait and send it
 * again up to kFlashPausesMost times; any other code ends the run */
inline void flash_refused_block(flash_run& r, uint8_t nrc)
{
    if (nrc == 0x73 && !r.resent) {
        r.resent = true;
        return;
    }
    if ((nrc == 0x92 || nrc == 0x93) && r.pauses < kFlashPausesMost) {
        ++r.pauses;
        r.paused = true;
        return;
    }
    flash_stop(r, flash_end::refused, nrc);
}

inline bool flash_moves_floor(const flash_run& r)
{
    return r.at == flash_stage::erase || r.at == flash_stage::open;
}

/* An answer to the request that is out */
inline flash_wait flash_reply(flash_run& r, const uint8_t* d, size_t n)
{
    if (!r.asked) {
        return flash_wait::go_on;
    }
    const uint8_t sid = flash_service(r);
    const bool negative = n == 3 && d[0] == 0x7F && d[1] == sid;
    if (negative && d[2] == 0x78) {
        r.floor = r.floor || flash_moves_floor(r);
        return flash_wait::keep_waiting;
    }
    r.asked = false;
    if (n == 0) {
        r.floor = r.floor || flash_moves_floor(r);
        flash_stop(r, flash_end::bad_reply);
        return flash_wait::go_on;
    }
    if (!negative) {
        r.floor = r.floor || flash_moves_floor(r);
        flash_positive(r, d, n);
    } else if (r.at == flash_stage::block && r.floor) {
        flash_refused_block(r, d[2]);
    } else {
        flash_stop(r, flash_end::refused, d[2]);
    }
    if (r.canceled && r.at != flash_stage::done) {
        flash_stop(r, flash_end::canceled);
    }
    return flash_wait::go_on;
}

/* No answer came: one that may have reached the module moves the floor */
inline void flash_silence(flash_run& r)
{
    if (!r.asked) {
        return;
    }
    r.asked = false;
    r.floor = r.floor || flash_moves_floor(r);
    flash_stop(r, r.canceled ? flash_end::canceled : flash_end::no_answer);
}

inline void flash_cancel(flash_run& r)
{
    if (r.at == flash_stage::done) {
        return;
    }
    if (r.asked) {
        r.canceled = true;
    } else {
        flash_stop(r, flash_end::canceled);
    }
}

} // namespace ecuio_operation

#endif /* ECUIO_OPERATION_FLASH_H */
