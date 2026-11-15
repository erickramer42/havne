// File/layer: app/alerttone.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge), the player through a fake
//          backend; Android's ToneGenerator on the phone (BENCH)
//
// The alert's tone (ADR-0063 section 9): one short tone, the two levels
// told apart by their count of beeps. The bridge asks at most once a tick
// and decides which firings sound; this plays them on a thread of its own,
// since Android's startTone waits for the audio track's first callback (up
// to 3 s in AOSP's ToneGenerator.cpp). A tone plays out before the next
// starts, the asks made meanwhile merged into the loudest level asked
// within the stale bound; an ask older than that when its turn comes plays
// nothing, and holds back no fresher ask of any level. A generator that
// can't be made or fails is made again on a later ask, at most once a
// retry interval. On Android the backend is android.media.ToneGenerator
// over JNI on the alarm stream; elsewhere there is none (the desktop's
// sound is v0.8.1's) and the tones are only counted.

#pragma once

#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

class AlertTone
{
public:
    // A tone maker. Each call is made on the player's thread.
    class Backend
    {
    public:
        virtual ~Backend() = default;
        // False: no generator, made again after the retry interval
        virtual bool make() = 0;
        // Starts the level's tone; false: the generator failed
        virtual bool start(uint32_t level) = 0;
        virtual void release() = 0;
    };
    static constexpr int ToneMs = 400; // longer than either tone
    struct Timing {
        int toneMs = ToneMs; // a tone's room before the next starts
        int staleMs = 1000;  // an ask older than this plays nothing
        int retryMs = 5000;  // between attempts at a failed generator
    };

    // The platform's backend: Android's ToneGenerator, none elsewhere
    AlertTone();
    AlertTone(std::unique_ptr<Backend> backend, Timing timing);
    // Releases the generator and joins the player's thread, so it waits
    // out a start under way: up to AOSP's 3 s, longer only if the audio
    // system hangs. Joined rather than left running, since a thread still
    // in JNI after Qt and the bridge are gone could crash the app's end.
    ~AlertTone();
    AlertTone(const AlertTone&) = delete;
    AlertTone& operator=(const AlertTone&) = delete;

    // Starts the player's thread, once: the bridge calls it at an attach
    // of a set that sounds. GUI thread.
    void prepare();
    // One tone at level (ECUIO_ALERT_WARNING or _DANGER; anything else
    // plays nothing). Never blocks on the backend and allocates nothing.
    // GUI thread.
    void play(uint32_t level);

    // The tones asked at each level, on every platform
    uint64_t playedForTesting(uint32_t level) const;
    // The asks the player dropped as stale
    uint64_t staleForTesting() const
    {
        return m_stale.load(std::memory_order_relaxed);
    }

private:
    using Clock = std::chrono::steady_clock;
    void run();

    std::unique_ptr<Backend> m_backend;
    Timing m_timing;
    std::atomic<uint64_t> m_played[3]{};
    std::atomic<uint64_t> m_stale{0};
    std::mutex m_mutex; // guards the asks waiting and the stop
    std::condition_variable m_wake;
    std::thread m_thread;
    // For each level, whether an ask waits and the newest one's time
    bool m_asked[3]{};
    Clock::time_point m_askedAt[3]{};
    bool m_stop = false;
    bool m_prepared = false;
};
