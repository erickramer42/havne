// File/layer: cli/line_ring.h | havne-cli, the worker-to-output ring (ADR-0031 §4)
// Testing: UNIT (tests/test_cli.cpp, CliRingTest)
//
// One ring of text between core's worker, which pushes finished lines,
// and the main thread, which drains them to standard output. Allocated
// once; a push copies bytes and never allocates. A full ring drops a
// live session's line and counts it, and the next line pushed after
// drops is preceded by a `dropped` line with the counts; a replay's push
// waits for room instead, until stop().

#ifndef HAVNE_CLI_LINE_RING_H
#define HAVNE_CLI_LINE_RING_H

#include "jsonl.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

namespace cli {

enum class LineKind { Frame, Values };

class LineRing
{
public:
    // False when the buffer can't be allocated
    bool init(size_t capacity)
    {
        m_buf.reset(new (std::nothrow) char[capacity]);
        m_cap = m_buf ? capacity : 0;
        return m_buf != nullptr;
    }
    size_t capacity() const { return m_cap; }

    // A finished line (its '\n' included). wait: block until there is
    // room or stop() is called. False when the line was dropped.
    bool push(LineKind kind, const char* line, size_t n, bool wait)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            char note[96];
            const size_t note_n = pending_note(note, sizeof note);
            if (note_n + n <= m_cap - m_size) {
                put(note, note_n);
                m_unreported_frames = m_unreported_values = 0;
                put(line, n);
                m_data.notify_one();
                return true;
            }
            if (!wait || m_stopped || note_n + n > m_cap) {
                break;
            }
            m_space.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (kind == LineKind::Frame) {
            ++m_unreported_frames;
            ++m_dropped_frames;
        } else {
            ++m_unreported_values;
            ++m_dropped_values;
        }
        return false;
    }

    // Waits up to `wait` for something to drain, then copies all of it
    // to out (at least capacity() bytes) and returns how many bytes
    size_t drain(char* out, std::chrono::milliseconds wait)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_size == 0) {
            m_data.wait_for(lock, wait,
                            [this] { return m_size > 0 || m_woken; });
        }
        m_woken = false;
        const size_t n = m_size;
        const size_t first = n < m_cap - m_head ? n : m_cap - m_head;
        std::memcpy(out, m_buf.get() + m_head, first);
        std::memcpy(out + first, m_buf.get(), n - first);
        m_head = (m_head + n) % (m_cap ? m_cap : 1);
        m_size = 0;
        m_space.notify_all();
        return n;
    }

    // Ends every wait in push, now and later: a waiting line is dropped
    void stop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopped = true;
        m_space.notify_all();
    }

    // Ends one wait in drain early
    void wake()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_woken = true;
        m_data.notify_one();
    }

    uint64_t dropped_frames() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_dropped_frames;
    }
    uint64_t dropped_values() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_dropped_values;
    }

private:
    // The `dropped` line owed before the next one, or 0 bytes
    size_t pending_note(char* buf, size_t cap) const
    {
        if (m_unreported_frames == 0 && m_unreported_values == 0) {
            return 0;
        }
        Line l(buf, cap);
        l.raw("{\"type\":\"dropped\"");
        l.key("frames");
        l.u64(m_unreported_frames);
        l.key("values");
        l.u64(m_unreported_values);
        l.ch('}');
        return l.finish() ? l.size() : 0;
    }

    void put(const char* p, size_t n)
    {
        size_t tail = (m_head + m_size) % m_cap;
        const size_t first = n < m_cap - tail ? n : m_cap - tail;
        std::memcpy(m_buf.get() + tail, p, first);
        std::memcpy(m_buf.get(), p + first, n - first);
        m_size += n;
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_data;
    std::condition_variable m_space;
    std::unique_ptr<char[]> m_buf;
    size_t m_cap = 0;
    size_t m_head = 0;
    size_t m_size = 0;
    bool m_stopped = false;
    bool m_woken = false;
    uint64_t m_unreported_frames = 0;
    uint64_t m_unreported_values = 0;
    uint64_t m_dropped_frames = 0;
    uint64_t m_dropped_values = 0;
};

} // namespace cli

#endif // HAVNE_CLI_LINE_RING_H
