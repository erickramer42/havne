// File/layer: tests/vcan_recording_ecu.h | test-only, C++17, Qt-free, Linux
//             only (SocketCAN); header-only
// Testing: HARNESS: SocketCanFaultTest (test_vcan)
//
// The test car's own replies answer a live session: each Service 01
// request gets the recording's reply for its PID at the ECU's time since
// it started (the recording plays at its own pace), a request of several
// PIDs J1979's packed answer, so long replies are First and Consecutive
// Frames. This ECU frames them itself, on a raw socket, instead of
// through the kernel's ISO-TP (vcan_bus::Ecu), so it can break them:
//   - silent: no reply at all;
//   - late: the whole reply after the request engine's wait;
//   - bad_sequence: a segmented reply's last Consecutive Frame with the
//     wrong sequence number;
//   - stalled: a segmented reply's last Consecutive Frame later than N_Cr.
// Each fault hits every Nth reply, single-frame and segmented replies
// counted apart (a segmented reply is a pack's), so a run is the same
// each time, a period never falls into step with the rotation's laps,
// and periods like 5 and 7 never break three packs in a row (a session
// whose packs fail that often goes back to one PID per request, as it
// should). The supported-PID bitmaps
// are never broken: the probe's walk ends at its first silence, which
// is its own rule. Every reply sent is logged with its fate, so a test
// compares what the session delivered with what the bus carried whole.

#ifndef ECUIO_TESTS_VCAN_RECORDING_ECU_H
#define ECUIO_TESTS_VCAN_RECORDING_ECU_H

#include "vcan_bus.h"

#include <linux/can/raw.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace vcan_bus {

class RecordingEcu
{
public:
    struct Faults {
        /* every Nth reply of its kind (single-frame, segmented); 0 = never */
        unsigned silent_every = 0;
        unsigned late_every = 0;
        /* every Nth segmented reply */
        unsigned bad_sequence_every = 0;
        unsigned stalled_every = 0;
    };
    enum class Fate { intact, late, silent, bad_sequence, stalled };
    struct Sent {
        std::vector<uint8_t> data; /* the reply, the service byte first */
        Fate fate = Fate::intact;
        bool segmented = false;
    };

    /* Later than the request engine's wait (ECUIO_ENGINE_WAIT_US, 100 ms)
     * and early enough for the next request's (sent as that wait ends) */
    static constexpr int kLateMs = 130;
    /* Later than N_Cr (150 ms) after the frame before, and early enough
     * for the next request's wait, sent 100 ms into this one's */
    static constexpr int kStalledMs = 160;

    RecordingEcu(const std::vector<Message>& recording, Faults faults,
                 const char* ifname = interface_name())
        : faults_(faults), raw_(open_raw(ifname)),
          stop_(::eventfd(0, EFD_CLOEXEC))
    {
        const int64_t first = recording.empty() ? 0 : recording.front().host_us;
        for (const Message& m : recording) {
            if (m.id == kReply && m.data.size() >= 2 && m.data[0] == 0x41) {
                pids_[m.data[1]].push_back(
                    {m.host_us - first,
                     std::vector<uint8_t>(m.data.begin() + 2, m.data.end())});
            }
        }
        can_filter only{kRequest, CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG};
        if (raw_.ok() && ::setsockopt(raw_.get(), SOL_CAN_RAW, CAN_RAW_FILTER,
                                      &only, sizeof(only)) != 0) {
            raw_.reset();
        }
        if (ok()) {
            start_ = std::chrono::steady_clock::now();
            thread_ = std::thread([this] { run(); });
        }
    }
    RecordingEcu(const RecordingEcu&) = delete;
    RecordingEcu& operator=(const RecordingEcu&) = delete;
    ~RecordingEcu() { stop(); }

    bool ok() const { return raw_.ok() && stop_.ok(); }
    /* Stops answering; what was sent stays readable */
    void stop()
    {
        if (thread_.joinable()) {
            const uint64_t one = 1;
            (void)!::write(stop_.get(), &one, sizeof(one));
            thread_.join();
        }
    }

    /* Every reply, in the order the requests came (a silent one never
     * went out) */
    std::vector<Sent> sent() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_;
    }
    /* The replies a receiver can take whole: intact or late */
    std::vector<std::vector<uint8_t>> whole() const
    {
        std::vector<std::vector<uint8_t>> out;
        for (const Sent& s : sent()) {
            if (s.fate == Fate::intact || s.fate == Fate::late) {
                out.push_back(s.data);
            }
        }
        return out;
    }
    unsigned count(Fate fate, bool segmented_only = false) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<unsigned>(
            std::count_if(sent_.begin(), sent_.end(), [&](const Sent& s) {
                return s.fate == fate && (s.segmented || !segmented_only);
            }));
    }
    /* The PIDs answered 7F 01 12: the car listed them, the recording
     * holds no reply for them */
    std::set<uint8_t> refused() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return refused_;
    }
    /* The PIDs answered with the recording's values, the bitmaps aside */
    std::set<uint8_t> answered() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return answered_;
    }
    /* Flow Control frames other than Clear To Send, block size 0 and
     * STmin 0, and segmented replies whose Flow Control never came */
    unsigned bad_flow_controls() const { return bad_fc_.load(); }
    unsigned unflowed() const { return unflowed_.load(); }

private:
    struct Reading {
        int64_t at_us;
        std::vector<uint8_t> data;
    };

    static bool bitmap_pid(uint8_t pid) { return pid % 0x20 == 0; }

    int64_t elapsed_us() const
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - start_)
            .count();
    }

    /* The recording's reply for a PID at the ECU's time: the last one at
     * or before it, or its first */
    const std::vector<uint8_t>* reading(uint8_t pid) const
    {
        const std::vector<Reading>& r = pids_[pid];
        if (r.empty()) {
            return nullptr;
        }
        const int64_t now = elapsed_us();
        auto it = std::upper_bound(
            r.begin(), r.end(), now,
            [](int64_t t, const Reading& x) { return t < x.at_us; });
        return it == r.begin() ? &r.front().data : &(it - 1)->data;
    }

    /* Waits ms, or less for the stop; false on the stop */
    bool pause(int ms)
    {
        pollfd p{stop_.get(), POLLIN, 0};
        return ::poll(&p, 1, ms) == 0;
    }

    /* The next frame to the ECU within ms (-1 waits); false on the stop
     * or the time running out (*stopped says which) */
    bool next_frame(can_frame* f, int ms, bool* stopped)
    {
        *stopped = false;
        for (;;) {
            pollfd p[2] = {{raw_.get(), POLLIN, 0}, {stop_.get(), POLLIN, 0}};
            const int n = ::poll(p, 2, ms);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0 || p[1].revents != 0) {
                *stopped = n != 0;
                return false;
            }
            if (::read(raw_.get(), f, sizeof(*f)) ==
                static_cast<ssize_t>(sizeof(*f))) {
                return true;
            }
        }
    }

    bool put(const uint8_t* bytes, size_t n)
    {
        can_frame f{};
        f.can_id = kReply;
        f.can_dlc = 8;
        std::memset(f.data, kPad, sizeof(f.data));
        std::memcpy(f.data, bytes, n);
        /* vcan has no flow control: a full send queue says ENOBUFS */
        while (::write(raw_.get(), &f, sizeof(f)) !=
               static_cast<ssize_t>(sizeof(f))) {
            if (errno != ENOBUFS) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return true;
    }

    /* A reply as ISO 15765-2 frames it, broken as fate says. False on the
     * stop. */
    bool send(const std::vector<uint8_t>& reply, Fate fate)
    {
        uint8_t b[8];
        if (reply.size() <= 7) {
            b[0] = static_cast<uint8_t>(reply.size());
            std::memcpy(b + 1, reply.data(), reply.size());
            return put(b, 1 + reply.size());
        }
        b[0] = static_cast<uint8_t>(0x10 | (reply.size() >> 8));
        b[1] = static_cast<uint8_t>(reply.size() & 0xFF);
        std::memcpy(b + 2, reply.data(), 6);
        if (!put(b, 8)) {
            return false;
        }
        /* The tester's Flow Control; a request meanwhile waits its turn */
        can_frame f;
        bool stopped = false;
        for (;;) {
            if (!next_frame(&f, 1000, &stopped)) {
                if (stopped) {
                    return false;
                }
                unflowed_.fetch_add(1);
                return true;
            }
            if ((f.data[0] >> 4) == 3) {
                break;
            }
            queued_.push_back(f);
        }
        if (f.can_dlc != 8 || f.data[0] != 0x30 || f.data[1] != 0 ||
            f.data[2] != 0) {
            bad_fc_.fetch_add(1);
        }
        uint8_t seq = 1;
        for (size_t at = 6; at < reply.size(); at += 7) {
            const size_t take = std::min<size_t>(7, reply.size() - at);
            const bool last = at + take == reply.size();
            b[0] = static_cast<uint8_t>(0x20 | seq);
            if (last && fate == Fate::bad_sequence) {
                b[0] = static_cast<uint8_t>(0x20 | ((seq + 1) & 0x0F));
            }
            if (last && fate == Fate::stalled && !pause(kStalledMs)) {
                return false;
            }
            std::memcpy(b + 1, reply.data() + at, take);
            if (!put(b, 1 + take)) {
                return false;
            }
            seq = static_cast<uint8_t>((seq + 1) & 0x0F);
        }
        return true;
    }

    /* The answer to a request, empty for none (a pack with nothing the
     * recording holds, as J1979's ECU says nothing) */
    std::vector<uint8_t> answer(const uint8_t* req, size_t n, bool* bitmap)
    {
        *bitmap = false;
        if (n < 2 || req[0] != 0x01) {
            return {0x7F, req[0], 0x11};
        }
        std::vector<uint8_t> out{0x41};
        for (size_t k = 1; k < n; ++k) {
            const std::vector<uint8_t>* r = reading(req[k]);
            if (n == 2 && r == nullptr) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!bitmap_pid(req[1])) {
                    refused_.insert(req[1]);
                }
                return {0x7F, 0x01, 0x12};
            }
            if (r != nullptr) {
                out.push_back(req[k]);
                out.insert(out.end(), r->begin(), r->end());
                if (!bitmap_pid(req[k])) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    answered_.insert(req[k]);
                }
            }
        }
        *bitmap = n == 2 && bitmap_pid(req[1]);
        return out.size() > 1 ? out : std::vector<uint8_t>{};
    }

    void run()
    {
        unsigned singles = 0;
        unsigned segmented = 0;
        const auto every = [](unsigned n, unsigned k) {
            return n != 0 && k % n == 0;
        };
        for (;;) {
            can_frame f;
            if (!queued_.empty()) {
                f = queued_.front();
                queued_.pop_front();
            } else {
                bool stopped = false;
                if (!next_frame(&f, -1, &stopped)) {
                    return;
                }
            }
            const uint8_t len = f.data[0];
            if ((len >> 4) != 0 || len == 0 || len > 7 || f.can_dlc < 1 + len) {
                continue; /* a requester's Flow Control, or not a request */
            }
            bool bitmap = false;
            const std::vector<uint8_t> reply = answer(f.data + 1, len, &bitmap);
            if (reply.empty()) {
                continue;
            }
            Sent s;
            s.data = reply;
            s.segmented = reply.size() > 7;
            if (!bitmap) {
                const unsigned k = s.segmented ? ++segmented : ++singles;
                if (every(faults_.silent_every, k)) {
                    s.fate = Fate::silent;
                } else if (every(faults_.late_every, k)) {
                    s.fate = Fate::late;
                } else if (s.segmented &&
                           every(faults_.bad_sequence_every, k)) {
                    s.fate = Fate::bad_sequence;
                } else if (s.segmented && every(faults_.stalled_every, k)) {
                    s.fate = Fate::stalled;
                }
            }
            if (s.fate == Fate::late && !pause(kLateMs)) {
                return;
            }
            if (s.fate != Fate::silent && !send(reply, s.fate)) {
                return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            sent_.push_back(std::move(s));
        }
    }

    Faults faults_;
    std::vector<Reading> pids_[256];
    Fd raw_;
    Fd stop_;
    std::chrono::steady_clock::time_point start_;
    std::deque<can_frame> queued_; /* the worker's own */
    mutable std::mutex mutex_;
    std::vector<Sent> sent_;
    std::set<uint8_t> refused_;
    std::set<uint8_t> answered_;
    std::atomic<unsigned> bad_fc_{0};
    std::atomic<unsigned> unflowed_{0};
    std::thread thread_;
};

} // namespace vcan_bus

#endif /* ECUIO_TESTS_VCAN_RECORDING_ECU_H */
