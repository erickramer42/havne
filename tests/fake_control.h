// File/layer: tests/fake_control.h | HARNESS: the fake DLL's control page
// Testing: shared by fake_j2534.cpp and test_j2534_health.cpp, which
//          drives one session through each backend
//
// A test process creates one named shared-memory page and names it in
// the environment (ECUIO_FAKE_CONTROL) before any session opens.
// Whichever fake instance serves the session reads it on every vendor
// call: the module loaded in the test process, or the copy inside the
// surrogate child, whose environment is frozen at spawn and which no
// test hook can reach. Through the page a test:
//   - sets and clears knobs mid-session; a knob on the page shadows the
//     environment variable of the same name,
//   - queues received messages with any RxStatus, and
//   - reads what the fake publishes: the process that opened it, its
//     count of successful writes, and how often it was disconnected and
//     closed by the IDs it handed out.
// Fixed-size fields and no pointers, so an x64 test process and an x86
// child map the same bytes.

#ifndef ECUIO_TESTS_FAKE_CONTROL_H
#define ECUIO_TESTS_FAKE_CONTROL_H

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fake_control {

constexpr const char* kEnvName = "ECUIO_FAKE_CONTROL";
constexpr uint32_t kMagic = 0x43464345u;   /* "ECFC" */
constexpr uint32_t kMaxKnobs = 16;
constexpr uint32_t kNameLen = 48;
constexpr uint32_t kValueLen = 32;
constexpr uint32_t kInboxDepth = 16;
constexpr uint32_t kMsgMax = 64;

struct Knob {
    char name[kNameLen];     /* empty: a free slot */
    char value[kValueLen];
};

struct Message {
    uint32_t size;
    uint32_t timestamp;
    uint32_t rx_status;      /* PASSTHRU_MSG.RxStatus, verbatim */
    uint8_t data[kMsgMax];
};

struct Page {
    uint32_t magic;
    volatile LONG lock;      /* spinlock over knobs and inbox */
    Knob knobs[kMaxKnobs];
    Message inbox[kInboxDepth];
    uint32_t inbox_len;
    volatile LONG writes;    /* PassThruWriteMsgs calls that succeeded */
    volatile LONG opener_pid; /* the process whose PassThruOpen ran last */
    volatile LONG disconnects; /* PassThruDisconnect calls naming the
                                * channel the fake handed out */
    volatile LONG closes;    /* PassThruClose calls naming its device */
};
static_assert(sizeof(Page) == 8 + kMaxKnobs * 80 + kInboxDepth * 76 + 20,
              "the page's layout must match in x86 and x64 builds");

class Guard {
public:
    explicit Guard(Page* p) : p_(p)
    {
        while (InterlockedCompareExchange(&p_->lock, 1, 0) != 0) {
            SwitchToThread();
        }
    }
    ~Guard() { InterlockedExchange(&p_->lock, 0); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    Page* p_;
};

inline void copy_bounded(char* dst, const char* src, size_t cap)
{
    size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* Fake side: the page the environment names, mapped once per module,
 * or nullptr. A miss isn't remembered, so a page created after the
 * module loaded is still found on a later call. */
inline Page* attach()
{
    static std::atomic<Page*> cached{nullptr};
    Page* page = cached.load();
    if (page != nullptr) {
        return page;
    }
    char name[96] = {};
    const DWORD n = GetEnvironmentVariableA(kEnvName, name, sizeof(name));
    if (n == 0 || n >= sizeof(name)) {
        return nullptr;
    }
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (mapping == nullptr) {
        return nullptr;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                               sizeof(Page));
    CloseHandle(mapping);    /* the view keeps the section alive */
    if (view == nullptr) {
        return nullptr;
    }
    page = static_cast<Page*>(view);
    if (page->magic != kMagic) {
        UnmapViewOfFile(view);
        return nullptr;
    }
    Page* expected = nullptr;
    if (!cached.compare_exchange_strong(expected, page)) {
        UnmapViewOfFile(view);   /* another thread mapped it first */
        return expected;
    }
    return page;
}

/* Test side: this process's page, created on first use as
 * Local\ecuio-fake-control-<pid> and named in the environment, so an
 * in-proc fake and any surrogate child spawned later both find it.
 * It lives until the process exits. */
inline Page* create_for_this_process()
{
    static Page* page = nullptr;
    if (page != nullptr) {
        return page;
    }
    char name[96];
    snprintf(name, sizeof(name), "Local\\ecuio-fake-control-%lu",
             static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_READWRITE, 0, sizeof(Page),
                                        name);
    if (mapping == nullptr) {
        return nullptr;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                               sizeof(Page));
    if (view == nullptr) {
        CloseHandle(mapping);
        return nullptr;
    }
    /* The mapping handle stays open with the view, for the process's
     * lifetime. */
    Page* p = static_cast<Page*>(view);
    memset(view, 0, sizeof(Page));
    p->magic = kMagic;
    SetEnvironmentVariableA(kEnvName, name);
    page = p;
    return page;
}

/* Copies a knob's value into buf. False when the page doesn't hold
 * the name. */
inline bool get(Page* p, const char* name, char* buf, size_t cap)
{
    Guard g(p);
    for (const Knob& k : p->knobs) {
        if (k.name[0] != '\0' && strncmp(k.name, name, kNameLen) == 0) {
            copy_bounded(buf, k.value, cap);
            return true;
        }
    }
    return false;
}

/* Sets a knob, or clears it when value is nullptr. False when a string
 * doesn't fit or every slot is taken. */
inline bool set(Page* p, const char* name, const char* value)
{
    if (strlen(name) >= kNameLen ||
        (value != nullptr && strlen(value) >= kValueLen)) {
        return false;
    }
    Guard g(p);
    Knob* free_slot = nullptr;
    for (Knob& k : p->knobs) {
        if (k.name[0] != '\0' && strncmp(k.name, name, kNameLen) == 0) {
            if (value == nullptr) {
                k.name[0] = '\0';
            } else {
                copy_bounded(k.value, value, kValueLen);
            }
            return true;
        }
        if (k.name[0] == '\0' && free_slot == nullptr) {
            free_slot = &k;
        }
    }
    if (value == nullptr) {
        return true;
    }
    if (free_slot == nullptr) {
        return false;
    }
    copy_bounded(free_slot->name, name, kNameLen);
    copy_bounded(free_slot->value, value, kValueLen);
    return true;
}

/* Clears every knob, the inbox and the published values. */
inline void reset(Page* p)
{
    Guard g(p);
    for (Knob& k : p->knobs) {
        k.name[0] = '\0';
    }
    p->inbox_len = 0;
    InterlockedExchange(&p->writes, 0);
    InterlockedExchange(&p->opener_pid, 0);
    InterlockedExchange(&p->disconnects, 0);
    InterlockedExchange(&p->closes, 0);
}

/* Queues a message for the fake's receive queue; it moves there on the
 * fake's next PassThruReadMsgs. False when the inbox is full or the
 * message is too long. */
inline bool post(Page* p, const uint8_t* data, uint32_t size,
                 uint32_t timestamp, uint32_t rx_status)
{
    if (size == 0 || size > kMsgMax) {
        return false;
    }
    Guard g(p);
    if (p->inbox_len >= kInboxDepth) {
        return false;
    }
    Message& m = p->inbox[p->inbox_len++];
    m.size = size;
    m.timestamp = timestamp;
    m.rx_status = rx_status;
    memcpy(m.data, data, size);
    return true;
}

/* Fake side: moves up to cap queued messages out, oldest first. */
inline uint32_t take(Page* p, Message* out, uint32_t cap)
{
    Guard g(p);
    uint32_t n = (p->inbox_len < cap) ? p->inbox_len : cap;
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = p->inbox[i];
    }
    for (uint32_t i = n; i < p->inbox_len; ++i) {
        p->inbox[i - n] = p->inbox[i];
    }
    p->inbox_len -= n;
    return n;
}

inline uint32_t writes(Page* p)
{
    return static_cast<uint32_t>(InterlockedCompareExchange(&p->writes, 0, 0));
}

inline unsigned long opener_pid(Page* p)
{
    return static_cast<unsigned long>(
        InterlockedCompareExchange(&p->opener_pid, 0, 0));
}

inline uint32_t disconnects(Page* p)
{
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&p->disconnects, 0, 0));
}

inline uint32_t closes(Page* p)
{
    return static_cast<uint32_t>(InterlockedCompareExchange(&p->closes, 0, 0));
}

} // namespace fake_control

#endif /* ECUIO_TESTS_FAKE_CONTROL_H */
