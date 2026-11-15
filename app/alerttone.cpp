// File/layer: app/alerttone.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge), the player through a fake
//          backend; Android's ToneGenerator on the phone (BENCH)

#include "alerttone.h"

#include "ecuio/conditions.h"

#include <initializer_list>
#include <system_error>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace {

uint32_t slotOf(uint32_t level)
{
    return level == ECUIO_ALERT_WARNING || level == ECUIO_ALERT_DANGER ? level
                                                                       : 0;
}

#ifdef Q_OS_ANDROID
// android.media.ToneGenerator. Each call makes a QJniEnvironment, which
// attaches the player's thread; Qt detaches it as the thread ends.
class ToneGeneratorBackend final : public AlertTone::Backend
{
public:
    bool make() override
    {
        QJniEnvironment env;
        constexpr const char* kTones = "android/media/ToneGenerator";
        // The alarm stream sounds with the phone set to silent and keeps
        // its own volume: the user asked for this sound in the file
        const jint stream = QJniObject::getStaticField<jint>(
            "android/media/AudioManager", "STREAM_ALARM");
        const jint volume =
            QJniObject::getStaticField<jint>(kTones, "MAX_VOLUME");
        m_warning = QJniObject::getStaticField<jint>(kTones, "TONE_PROP_BEEP");
        m_danger = QJniObject::getStaticField<jint>(kTones, "TONE_PROP_BEEP2");
        // Its constructor throws when the audio system refuses a track
        QJniObject tone(kTones, stream, volume); // ToneGenerator(int, int)
        if (env.checkAndClearExceptions() || !tone.isValid()) {
            return false;
        }
        m_tone = tone;
        return true;
    }
    bool start(uint32_t level) override
    {
        QJniEnvironment env;
        const jboolean started = m_tone.callMethod<jboolean>(
            "startTone", "(II)Z",
            level == ECUIO_ALERT_DANGER ? m_danger : m_warning,
            jint{AlertTone::ToneMs});
        return !env.checkAndClearExceptions() && started;
    }
    void release() override
    {
        QJniEnvironment env;
        if (m_tone.isValid()) {
            m_tone.callMethod<void>("release");
        }
        env.checkAndClearExceptions();
        m_tone = QJniObject();
    }

private:
    QJniObject m_tone;
    jint m_warning = 0;
    jint m_danger = 0;
};
#endif

std::unique_ptr<AlertTone::Backend> platformBackend()
{
#ifdef Q_OS_ANDROID
    return std::make_unique<ToneGeneratorBackend>();
#else
    return nullptr;
#endif
}

} // namespace

AlertTone::AlertTone() : AlertTone(platformBackend(), Timing{}) {}

AlertTone::AlertTone(std::unique_ptr<Backend> backend, Timing timing)
    : m_backend(std::move(backend)), m_timing(timing)
{}

AlertTone::~AlertTone()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_one();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void AlertTone::prepare()
{
    if (m_backend == nullptr || m_prepared) {
        return;
    }
    m_prepared = true;
    try {
        m_thread = std::thread([this]() { run(); });
    } catch (const std::system_error&) {
        qWarning("AlertTone: the tone's thread didn't start: no tone plays");
    }
}

void AlertTone::play(uint32_t level)
{
    const uint32_t slot = slotOf(level);
    if (slot == 0) {
        return;
    }
    m_played[slot].fetch_add(1, std::memory_order_relaxed);
    if (m_backend == nullptr) {
        return;
    }
    prepare();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_asked[slot] = true;
        m_askedAt[slot] = Clock::now();
    }
    m_wake.notify_one();
}

uint64_t AlertTone::playedForTesting(uint32_t level) const
{
    const uint32_t slot = slotOf(level);
    return slot == 0 ? 0 : m_played[slot].load(std::memory_order_relaxed);
}

void AlertTone::run()
{
    const auto ms = [](int n) { return std::chrono::milliseconds(n); };
    bool made = false;
    bool failing = false;
    Clock::time_point retryAt{};
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_wake.wait(lock, [this]() {
            return m_stop || m_asked[ECUIO_ALERT_WARNING] ||
                   m_asked[ECUIO_ALERT_DANGER];
        });
        if (m_stop) {
            break;
        }
        // The loudest level asked within the stale bound
        const Clock::time_point now = Clock::now();
        uint32_t level = 0;
        for (const uint32_t l : {ECUIO_ALERT_DANGER, ECUIO_ALERT_WARNING}) {
            if (!m_asked[l]) {
                continue;
            }
            m_asked[l] = false;
            if (now - m_askedAt[l] > ms(m_timing.staleMs)) {
                m_stale.fetch_add(1, std::memory_order_relaxed);
            } else if (level == 0) {
                level = l;
            }
        }
        lock.unlock();
        bool played = false;
        bool failed = false;
        if (level != 0) {
            if (!made && now >= retryAt) {
                made = m_backend->make();
                failed = !made;
            }
            if (made) {
                played = m_backend->start(level);
                if (!played) {
                    m_backend->release();
                    made = false;
                    failed = true;
                }
            }
            if (failed) {
                retryAt = Clock::now() + ms(m_timing.retryMs);
            }
        }
        if (failed && !failing) {
            qWarning("AlertTone: the tone generator failed; it is made "
                     "again on a later alert");
        } else if (played && failing) {
            qWarning("AlertTone: the tone generator was made again");
        }
        failing = failed || (failing && !played);
        lock.lock();
        if (played) {
            // The tone plays out: asks meanwhile merge into the loudest
            m_wake.wait_for(lock, ms(m_timing.toneMs),
                            [this]() { return m_stop; });
        }
    }
    lock.unlock();
    if (made) {
        m_backend->release();
    }
}
