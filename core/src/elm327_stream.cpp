// File/layer: core/src/elm327_stream.cpp | C++17, Qt-free, every platform
// Testing: HARNESS — Elm327StreamTest (test_elm327): a session through a
//          registered stream with a pump thread as the app, its failure
//          and reopen, the open's refusals and its wait, the registry's
//          rules, the buffer's overflow; UNIT — test_c_abi: stream.h as
//          strict C; Vehicle — the MX+ through Android (ADR-0028 §4)

#include "elm327_stream.h"
#include "elm327_port.h" /* ELM327_PORT_NAME_MAX */

#include "ecuio/stream.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

/* One registered stream. held belongs to the registry's lock, so an
 * unregister and an open can't cross; the rest to the stream's own. */
struct ecuio_stream {
    char name[ELM327_PORT_NAME_MAX + 1] = {};
    ecuio_stream_callbacks_t cb{};
    bool held = false; /* a session's port has it, open or opening */

    std::mutex lock;
    std::condition_variable changed;
    std::unique_ptr<uint8_t[]> buf; /* ELM327_STREAM_BUFFER, a ring */
    uint32_t head = 0;              /* the oldest byte not read */
    uint32_t len = 0;
    /* Between open and close the app's pushes are taken */
    bool accepting = false;
    bool opened = false;
    bool failed = false;
    bool overflowed = false;
    char reason[128] = {};
};

namespace {

/* The registry: every stream by name, under one lock */
struct registry {
    std::mutex lock;
    std::vector<ecuio_stream*> streams;
};

registry& streams()
{
    static registry r;
    return r;
}

/* ecuio_elm327_set_stream_open_ms's wait; 0 is ELM327_STREAM_OPEN_MS */
std::atomic<uint32_t> g_open_ms{0};

/* The stop flag is polled this often during an open's wait */
constexpr uint32_t ELM327_STREAM_STOP_POLL_MS = 20;

void set_why(char* why, uint32_t cap, const char* text)
{
    if (cap > 0) {
        snprintf(why, cap, "%s", text);
    }
}

/* The registered stream called name (the registry's lock held) */
ecuio_stream* find(registry& r, const char* name)
{
    for (ecuio_stream* s : r.streams) {
        if (strcmp(s->name, name) == 0) {
            return s;
        }
    }
    return nullptr;
}

/* The link is done with: pushes discarded from here, the app told to
 * disconnect, the stream free for the next session. */
void end_link(ecuio_stream* s)
{
    {
        std::lock_guard<std::mutex> g(s->lock);
        s->accepting = false;
        s->opened = s->failed = s->overflowed = false;
        s->head = s->len = 0;
    }
    s->cb.close(s->cb.user);
    registry& r = streams();
    std::lock_guard<std::mutex> g(r.lock);
    s->held = false;
}

/* The port's ops: the app's link, as the worker sees it */
bool stream_write(void* impl, const uint8_t* data, uint32_t len)
{
    auto* s = static_cast<ecuio_stream*>(impl);
    {
        std::lock_guard<std::mutex> g(s->lock);
        if (s->failed || s->overflowed) {
            return false;
        }
    }
    /* unlocked: the app may push from inside its write */
    s->cb.write(s->cb.user, data, len);
    return true;
}

int32_t stream_read(void* impl, uint8_t* out, uint32_t cap, uint32_t timeout_ms)
{
    auto* s = static_cast<ecuio_stream*>(impl);
    std::unique_lock<std::mutex> g(s->lock);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        /* Lost text can't be resynchronized: the link is as good as gone */
        if (s->failed || s->overflowed) {
            return -1;
        }
        if (s->len > 0) {
            const uint32_t n = (s->len < cap) ? s->len : cap;
            for (uint32_t i = 0; i < n; ++i) {
                out[i] = s->buf[(s->head + i) % ELM327_STREAM_BUFFER];
            }
            s->head = (s->head + n) % ELM327_STREAM_BUFFER;
            s->len -= n;
            return static_cast<int32_t>(n);
        }
        if (s->changed.wait_until(g, deadline) == std::cv_status::timeout &&
            s->len == 0 && !s->failed && !s->overflowed) {
            return 0;
        }
    }
}

void stream_close(void* impl)
{
    end_link(static_cast<ecuio_stream*>(impl));
}

const ecuio_elm327_port_ops_t kStreamPortOps = {stream_write, stream_read,
                                                stream_close};

} // namespace

bool elm327_stream_name(const char* name, size_t n)
{
    if (n == 0 || n > ELM327_PORT_NAME_MAX) {
        return false;
    }
    size_t i = 0;
    while (i < n && name[i] >= 'a' && name[i] <= 'z') {
        ++i;
    }
    if (i == 0 || i >= n || name[i] != ':' || i + 1 == n) {
        return false;
    }
    for (++i; i < n; ++i) {
        const char c = name[i];
        if (c <= ' ' || c > '~' || c == '?') {
            return false;
        }
    }
    return true;
}

ecuio_status_t elm327_stream_open(const char* name,
                                  const std::atomic<bool>* stop,
                                  ecuio_elm327_port_t* out, char* why,
                                  uint32_t why_cap)
{
    ecuio_stream* s = nullptr;
    {
        registry& r = streams();
        std::lock_guard<std::mutex> g(r.lock);
        s = find(r, name);
        if (s == nullptr) {
            set_why(why, why_cap, "no such port");
            return ECUIO_ERR_LOAD_FAILED;
        }
        if (s->held) {
            set_why(why, why_cap, "the link is in use by another session");
            return ECUIO_ERR_LOAD_FAILED;
        }
        s->held = true;
    }
    {
        std::lock_guard<std::mutex> g(s->lock);
        s->head = s->len = 0;
        s->opened = s->failed = s->overflowed = false;
        s->reason[0] = '\0';
        s->accepting = true;
    }
    /* unlocked: the app may answer from inside its open */
    s->cb.open(s->cb.user);

    const uint32_t wait_ms =
        (g_open_ms.load() != 0) ? g_open_ms.load() : ELM327_STREAM_OPEN_MS;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    bool ok = false;
    {
        std::unique_lock<std::mutex> g(s->lock);
        for (;;) {
            if (s->failed) {
                set_why(why, why_cap,
                        s->reason[0] != '\0' ? s->reason : "the link failed");
                break;
            }
            if (s->opened) {
                ok = true;
                break;
            }
            if (stop != nullptr && stop->load()) {
                set_why(why, why_cap, "the session closed");
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                if (why_cap > 0) {
                    snprintf(why, why_cap, "the link didn't open within %u s",
                             static_cast<unsigned>((wait_ms + 999) / 1000));
                }
                break;
            }
            const auto slice =
                std::chrono::milliseconds(ELM327_STREAM_STOP_POLL_MS);
            s->changed.wait_for(g, (deadline - now < slice) ? deadline - now
                                                            : slice);
        }
    }
    if (!ok) {
        end_link(s);
        return ECUIO_ERR_LOAD_FAILED;
    }
    out->ops = &kStreamPortOps;
    out->impl = s;
    return ECUIO_OK;
}

extern "C" ecuio_status_t
ecuio_stream_register(const char* name, const ecuio_stream_callbacks_t* cb,
                      ecuio_stream_t** out)
{
    if (name == nullptr || cb == nullptr || out == nullptr ||
        cb->open == nullptr || cb->write == nullptr || cb->close == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    bool zero = cb->flags == 0;
    for (uint32_t word : cb->reserved) {
        zero = zero && word == 0;
    }
    if (!zero || !elm327_stream_name(name, strlen(name))) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    std::unique_ptr<ecuio_stream> s(new (std::nothrow) ecuio_stream());
    if (s == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    s->buf.reset(new (std::nothrow) uint8_t[ELM327_STREAM_BUFFER]);
    if (s->buf == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->cb = *cb;

    registry& r = streams();
    std::lock_guard<std::mutex> g(r.lock);
    if (find(r, name) != nullptr) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    try {
        r.streams.push_back(s.get());
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    *out = s.release();
    return ECUIO_OK;
}

extern "C" ecuio_status_t ecuio_stream_unregister(ecuio_stream_t* stream)
{
    if (stream == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    {
        registry& r = streams();
        std::lock_guard<std::mutex> g(r.lock);
        if (stream->held) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        for (auto it = r.streams.begin(); it != r.streams.end(); ++it) {
            if (*it == stream) {
                r.streams.erase(it);
                break;
            }
        }
    }
    delete stream;
    return ECUIO_OK;
}

extern "C" void ecuio_stream_opened(ecuio_stream_t* stream)
{
    if (stream == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> g(stream->lock);
        if (!stream->accepting || stream->failed) {
            return;
        }
        stream->opened = true;
    }
    stream->changed.notify_all();
}

extern "C" void ecuio_stream_received(ecuio_stream_t* stream,
                                      const uint8_t* data, uint32_t len)
{
    if (stream == nullptr || data == nullptr || len == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> g(stream->lock);
        if (!stream->accepting || stream->failed || stream->overflowed) {
            return;
        }
        const uint32_t room = ELM327_STREAM_BUFFER - stream->len;
        const uint32_t n = (len < room) ? len : room;
        uint32_t at = (stream->head + stream->len) % ELM327_STREAM_BUFFER;
        for (uint32_t i = 0; i < n; ++i) {
            stream->buf[at] = data[i];
            at = (at + 1) % ELM327_STREAM_BUFFER;
        }
        stream->len += n;
        stream->overflowed = n < len;
    }
    stream->changed.notify_all();
}

extern "C" void ecuio_stream_failed(ecuio_stream_t* stream, const char* reason)
{
    if (stream == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> g(stream->lock);
        if (!stream->accepting || stream->failed) {
            return;
        }
        stream->failed = true;
        /* One line: a control character is a space */
        size_t i = 0;
        if (reason != nullptr) {
            for (; reason[i] != '\0' && i + 1 < sizeof(stream->reason); ++i) {
                const auto c = static_cast<unsigned char>(reason[i]);
                stream->reason[i] = (c < 0x20 || c == 0x7F) ? ' ' : reason[i];
            }
        }
        stream->reason[i] = '\0';
    }
    stream->changed.notify_all();
}

extern "C" void ecuio_elm327_set_stream_open_ms(uint32_t ms)
{
    g_open_ms.store(ms);
}
