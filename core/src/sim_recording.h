// File/layer: core/src/sim_recording.h | internal, C++17, Qt-free;
//             ALL platforms; the loader in sim.cpp
// Testing: UNIT: SimRecordingTest (test_obd2): replies kept by the
//          request they answer, packed replies split, each answer at its
//          time (before the first, between, held after the last, looped),
//          packs, the car's bitmaps and the ones worked out without them,
//          the VIN given, the refusals; the loader on the committed
//          fixture and its refusals; HARNESS: SimBusTest (test_vcan,
//          Linux): an obd2 session against a recording on vcan0
//
// A recording as the simulated ECU (ADR-0046 section 7): the replies the
// engine ECU (0x7E8) sent in a recording's frames.csv, each kept under
// the request it answers (41 <PID> under 01 <PID>, a packed 41 split into
// its PIDs first, 42 <PID> <frame> under 02 <PID> <frame>, 43, 47 and 4A
// under 03, 07 and 0A, 49 <item> under 09 <item>, 62 <DID> under
// 22 <DID>), with its time since the recording started. A request gets
// the reply the recording held for it at the simulated ECU's time: the
// last at or before it, its first before that, its last after the
// recording ends (or, looping, the time taken round the recording's
// span). Pure: the file is read by ecuio_sim_load_recording (sim.cpp).

#ifndef ECUIO_SIM_RECORDING_H
#define ECUIO_SIM_RECORDING_H

#include "ecuio/core.h"
#include "obd2_reply.h"
#include "recording_stretch.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

/* The engine ECU's replies, the only ones kept */
constexpr uint32_t ECUIO_SIM_RECORDING_ECU = 0x7E8u;

/* The longest reply kept: ISO-TP's longest message */
constexpr uint32_t ECUIO_SIM_RECORDING_REPLY_MAX = 4095;

/* One reply: its time since the recording started and its bytes' place
 * in the recording's store */
struct ecuio_sim_take {
    int64_t at_us;
    uint32_t offset;
    uint32_t len;
};

struct ecuio_sim_recording {
    /* Each request's replies in time order, keyed by ecuio_sim_key */
    std::map<uint32_t, std::vector<ecuio_sim_take>> replies;
    std::vector<uint8_t> bytes; /* every kept reply, back to back */
    int64_t span_us = 0;        /* the last kept reply's time */
    bool loop = false;          /* round the span, not held at its end */
    bool bitmaps = false;       /* the car's own: a 41 of a bitmap PID */
    uint8_t learned[256] = {};  /* ecuio_obd2_split's lengths */
};

/* A request's key: its length and up to three bytes, the service first */
inline uint32_t ecuio_sim_key(const uint8_t* b, uint32_t n)
{
    uint32_t k = n << 24;
    for (uint32_t i = 0; i < n && i < 3; ++i) {
        k |= static_cast<uint32_t>(b[i]) << (16 - 8 * i);
    }
    return k;
}

namespace ecuio_sim_rec_detail {

inline bool bitmap_pid(uint8_t pid)
{
    return pid % 0x20 == 0;
}

/* Keeps reply (len bytes) under the request key at at_us; may throw
 * bad_alloc */
inline void keep(ecuio_sim_recording& r, uint32_t key, int64_t at_us,
                 const uint8_t* reply, uint32_t len)
{
    const ecuio_sim_take t{at_us, static_cast<uint32_t>(r.bytes.size()), len};
    r.bytes.insert(r.bytes.end(), reply, reply + len);
    r.replies[key].push_back(t);
    if (at_us > r.span_us) {
        r.span_us = at_us;
    }
}

} // namespace ecuio_sim_rec_detail

/* One recorded frame at at_us (the recording's session time since its
 * first row): kept when it's the engine ECU's whole positive reply to a
 * request the simulated ECU answers. May throw bad_alloc. */
inline void ecuio_sim_recording_take(ecuio_sim_recording& r, int64_t at_us,
                                     const ecuio_frame_t& f)
{
    using namespace ecuio_sim_rec_detail;
    constexpr uint32_t kSkip = ECUIO_FRAME_FLAG_EXTENDED |
                               ECUIO_FRAME_FLAG_ERROR |
                               ECUIO_FRAME_FLAG_TRUNCATED;
    const uint8_t* d = f.data;
    const uint32_t n = f.data_length;
    if (f.arbitration_id != ECUIO_SIM_RECORDING_ECU || (f.flags & kSkip) != 0 ||
        n == 0 || n > ECUIO_SIM_RECORDING_REPLY_MAX) {
        return;
    }
    uint8_t req[3] = {static_cast<uint8_t>(d[0] & ~0x40u), 0, 0};
    switch (d[0]) {
    case 0x41: {
        if (n < 2) {
            return;
        }
        ecuio_obd2_record recs[ECUIO_OBD2_MAX_RECORDS];
        uint32_t count = 0;
        if (ecuio_obd2_split(d, static_cast<uint16_t>(n), r.learned, recs,
                             &count) == ecuio_obd2_reply::split) {
            for (uint32_t i = 0; i < count; ++i) {
                uint8_t one[2 + 64];
                if (recs[i].bytes > sizeof one - 2) {
                    continue;
                }
                one[0] = 0x41;
                one[1] = recs[i].pid;
                std::memcpy(one + 2, d + recs[i].start, recs[i].bytes);
                req[1] = recs[i].pid;
                r.bitmaps = r.bitmaps || bitmap_pid(recs[i].pid);
                keep(r, ecuio_sim_key(req, 2), at_us, one, 2u + recs[i].bytes);
            }
            return;
        }
        req[1] = d[1];
        r.bitmaps = r.bitmaps || bitmap_pid(d[1]);
        keep(r, ecuio_sim_key(req, 2), at_us, d, n);
        return;
    }
    case 0x42: /* 42 <PID> <frame> ... */
    case 0x62: /* 62 <DID hi> <DID lo> ... */
        if (n < 3) {
            return;
        }
        req[1] = d[1];
        req[2] = d[2];
        keep(r, ecuio_sim_key(req, 3), at_us, d, n);
        return;
    case 0x43:
    case 0x47:
    case 0x4A:
        keep(r, ecuio_sim_key(req, 1), at_us, d, n);
        return;
    case 0x49: /* 49 <item> ... */
        if (n < 2) {
            return;
        }
        req[1] = d[1];
        keep(r, ecuio_sim_key(req, 2), at_us, d, n);
        return;
    default:
        return; /* a refusal, a clear's 44, a service it doesn't answer */
    }
}

/* The reply the recording holds for key at now_us, or NULL (and *len 0) */
inline const uint8_t* ecuio_sim_recording_at(const ecuio_sim_recording& r,
                                             uint32_t key, int64_t now_us,
                                             uint32_t* len)
{
    *len = 0;
    const auto it = r.replies.find(key);
    if (it == r.replies.end() || it->second.empty()) {
        return nullptr;
    }
    const std::vector<ecuio_sim_take>& v = it->second;
    const int64_t t = r.loop && r.span_us > 0 ? now_us % r.span_us : now_us;
    auto at = std::upper_bound(
        v.begin(), v.end(), t,
        [](int64_t x, const ecuio_sim_take& y) { return x < y.at_us; });
    const ecuio_sim_take& take = at == v.begin() ? v.front() : *(at - 1);
    *len = take.len;
    return r.bytes.data() + take.offset;
}

namespace ecuio_sim_rec_detail {

/* A Service 01 PID's reply in the recording (41 <PID> data) */
inline const uint8_t* pid_reply(const ecuio_sim_recording& r, uint8_t pid,
                                int64_t now_us, uint32_t* len)
{
    const uint8_t req[2] = {0x01, pid};
    return ecuio_sim_recording_at(r, ecuio_sim_key(req, 2), now_us, len);
}

/* A data PID the recording holds a reply for */
inline bool has_data_pid(const ecuio_sim_recording& r, unsigned pid)
{
    if (pid > 0xFF || bitmap_pid(static_cast<uint8_t>(pid))) {
        return false;
    }
    const uint8_t req[2] = {0x01, static_cast<uint8_t>(pid)};
    return r.replies.count(ecuio_sim_key(req, 2)) != 0;
}

/* A recording with none of the car's bitmaps: the bitmap's bits worked
 * out from the PIDs it holds, as sim_ecu.h's are (J1979: each bitmap's
 * last bit names the next); 00 always */
inline bool worked_out_pid(const ecuio_sim_recording& r, unsigned pid)
{
    if (pid > 0xFF) {
        return false;
    }
    if (!bitmap_pid(static_cast<uint8_t>(pid))) {
        return has_data_pid(r, pid);
    }
    if (pid == 0) {
        return true;
    }
    for (unsigned p = pid + 1; p <= 0xFF; ++p) {
        if (has_data_pid(r, p)) {
            return true;
        }
    }
    return false;
}

inline uint32_t refuse(uint8_t service, uint8_t nrc, uint8_t* out)
{
    out[0] = 0x7F;
    out[1] = service;
    out[2] = nrc;
    return 3;
}

/* The recording's reply for a whole request key, else refused nrc */
inline uint32_t recorded(const ecuio_sim_recording& r, const uint8_t* req,
                         uint32_t len, int64_t now_us, uint8_t nrc,
                         uint8_t* out)
{
    uint32_t n = 0;
    const uint8_t* reply =
        ecuio_sim_recording_at(r, ecuio_sim_key(req, len), now_us, &n);
    if (reply == nullptr) {
        return refuse(req[0], nrc, out);
    }
    std::memcpy(out, reply, n);
    return n;
}

} // namespace ecuio_sim_rec_detail

/* The recording's reply to req (len bytes, the service byte first) at
 * now_us into out (ECUIO_SIM_RECORDING_REPLY_MAX bytes), its length; 0
 * for none. vin, when not NULL, is the 17 characters 09 02 answers
 * instead of the recording's. */
inline uint32_t ecuio_sim_recording_answer(const ecuio_sim_recording& r,
                                           int64_t now_us, const uint8_t* req,
                                           uint32_t len, const char* vin,
                                           uint8_t* out)
{
    using namespace ecuio_sim_rec_detail;
    if (len == 0) {
        return 0;
    }
    const uint8_t service = req[0];
    if (service == 0x01 && len == 2) {
        const uint8_t pid = req[1];
        uint32_t n = 0;
        const uint8_t* reply = pid_reply(r, pid, now_us, &n);
        if (reply != nullptr) {
            std::memcpy(out, reply, n);
            return n;
        }
        if (!bitmap_pid(pid) || r.bitmaps || !worked_out_pid(r, pid)) {
            return refuse(0x01, 0x12, out);
        }
        out[0] = 0x41;
        out[1] = pid;
        std::memset(out + 2, 0, 4);
        for (unsigned k = 1; k <= 32; ++k) {
            if (worked_out_pid(r, pid + k)) {
                out[2 + (k - 1) / 8] = static_cast<uint8_t>(
                    out[2 + (k - 1) / 8] | (0x80u >> ((k - 1) % 8)));
            }
        }
        return 6;
    }
    if (service == 0x01 && len >= 3 && len <= 7) {
        /* J1979's pack: each PID's record as the recording holds it now,
         * in the order asked; none it holds answers nothing */
        uint32_t n = 1;
        out[0] = 0x41;
        for (uint32_t k = 1; k < len; ++k) {
            uint32_t m = 0;
            const uint8_t* reply = pid_reply(r, req[k], now_us, &m);
            if (reply != nullptr &&
                n + m - 1 <= ECUIO_SIM_RECORDING_REPLY_MAX) {
                std::memcpy(out + n, reply + 1, m - 1);
                n += m - 1;
            }
        }
        return n > 1 ? n : 0;
    }
    if (service == 0x09 && len == 2 && req[1] == 0x02 && vin != nullptr) {
        out[0] = 0x49;
        out[1] = 0x02;
        out[2] = 0x01;
        std::memcpy(out + 3, vin, 17);
        return 3 + 17;
    }
    if ((service == 0x02 && len == 3) || (service == 0x09 && len == 2) ||
        ((service == 0x03 || service == 0x07 || service == 0x0A) && len == 1)) {
        return recorded(r, req, len, now_us, 0x12, out);
    }
    if (service == 0x22) {
        return len == 3 ? recorded(r, req, len, now_us, 0x31, out)
                        : refuse(0x22, 0x13, out);
    }
    if (service == 0x04) {
        /* A recording can't say what the car answers after a clear:
         * conditions not correct, as an ECU that won't clear says */
        return refuse(0x04, 0x22, out);
    }
    return refuse(service, 0x11, out);
}

/* Reads a recording's frames.csv (path: the file, or the folder holding
 * it; UTF-8, absolute) into *out, which it replaces: the stretch's rows
 * alone (recording_stretch.h; the whole file by default), its first at 0.
 * LOAD_FAILED with the detail set for a file that won't open or a row
 * that won't parse (its line number, never the path), a stretch with no
 * frames, or no reply from the engine ECU; OUT_OF_MEMORY. In sim.cpp. */
ecuio_status_t
ecuio_sim_load_recording(const std::string& path, ecuio_sim_recording* out,
                         const ecuio_stretch::stretch& stretch = {});

#endif /* ECUIO_SIM_RECORDING_H */
