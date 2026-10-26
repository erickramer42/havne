// File/layer: tests/fake_control.h | HARNESS: the fake DLL's control page
// Testing: shared by fake_j2534.cpp and every test that drives the fake
//          (the J2534 suites, test_bridge), whichever backend serves it
//
// A test process creates one named shared-memory page and names it in
// the environment (ECUIO_FAKE_CONTROL) before any session opens.
// Whichever fake instance serves the session reads it on every vendor
// call: the module loaded in the test process, or the copy inside the
// surrogate child, whose environment is frozen at spawn and which no
// test hook can reach. The page is the only way a test reaches the
// fake, so a test reads the same on either backend. Through it a test:
//   - sets and clears knobs mid-session; a knob on the page shadows the
//     environment variable of the same name,
//   - queues received messages with any RxStatus, and
//   - reads what the fake publishes: the process that opened it, its
//     successful writes (counted, the first kWriteDepth kept whole), the
//     last connect's parameters, the last filter started and stopped,
//     and how often it was disconnected and closed by the IDs it handed
//     out.
// Fixed-size fields and no pointers, so an x64 test process and an x86
// child map the same bytes; the size is pinned below and the layout
// versioned (kLayout).

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
/* Raised with any change to Page. A fake whose layout differs (a
 * build-win32 fake older than the tests) ignores the page, as it
 * ignores a wrong magic, and a test sees opener() stay 0. */
constexpr uint32_t kLayout = 2;
constexpr uint32_t kMaxKnobs = 16;
constexpr uint32_t kNameLen = 48;
constexpr uint32_t kValueLen = 32;
constexpr uint32_t kInboxDepth = 32;   /* a read's indication cap and more */
constexpr uint32_t kMsgMax = 64;
constexpr uint32_t kWriteDepth = 16;   /* the first writes, bytes kept */
constexpr uint32_t kFilterMax = 12;    /* a filter's mask and pattern */

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

struct Write {
    uint32_t size;
    uint32_t tx_flags;       /* PASSTHRU_MSG.TxFlags, verbatim */
    uint8_t data[kMsgMax];
};

struct Filter {
    uint32_t id;             /* the ID the fake handed out; 0 = none */
    uint32_t type;           /* PASS_FILTER, BLOCK_FILTER, FLOW_CONTROL_FILTER */
    uint32_t len;            /* of mask and pattern alike */
    uint8_t mask[kFilterMax];
    uint8_t pattern[kFilterMax];
};

struct Page {
    uint32_t magic;
    uint32_t layout;         /* kLayout */
    volatile LONG lock;      /* spinlock over everything below but the
                              * LONG counters */
    Knob knobs[kMaxKnobs];
    Message inbox[kInboxDepth];
    uint32_t inbox_len;
    volatile LONG writes;    /* PassThruWriteMsgs calls that succeeded */
    volatile LONG opener_pid; /* the process whose PassThruOpen ran last */
    volatile LONG disconnects; /* PassThruDisconnect calls naming the
                                * channel the fake handed out */
    volatile LONG closes;    /* PassThruClose calls naming its device */
    Write recorded[kWriteDepth]; /* the first writes since the reset */
    uint32_t connect_protocol; /* the last PassThruConnect, even refused */
    uint32_t connect_flags;
    uint32_t connect_baud;
    Filter last_filter;      /* the last filter started */
    uint32_t last_stopped;   /* the last filter ID stopped; 0 = none */
};
static_assert(sizeof(Page) == 12 + kMaxKnobs * 80 + kInboxDepth * 76 + 20 +
                                  kWriteDepth * 72 + 12 + 36 + 4,
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
    if (page->magic != kMagic || page->layout != kLayout) {
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
    p->layout = kLayout;
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
    memset(p->recorded, 0, sizeof(p->recorded));
    p->connect_protocol = 0;
    p->connect_flags = 0;
    p->connect_baud = 0;
    memset(&p->last_filter, 0, sizeof(p->last_filter));
    p->last_stopped = 0;
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

/* Fake side: a successful write, counted, and its bytes kept while
 * fewer than kWriteDepth are. */
inline void record_write(Page* p, const uint8_t* data, uint32_t size,
                         uint32_t tx_flags)
{
    Guard g(p);
    const LONG i = InterlockedCompareExchange(&p->writes, 0, 0);
    if (i >= 0 && static_cast<uint32_t>(i) < kWriteDepth) {
        Write& w = p->recorded[i];
        w.size = (size < kMsgMax) ? size : kMsgMax;
        w.tx_flags = tx_flags;
        memcpy(w.data, data, w.size);
    }
    InterlockedIncrement(&p->writes);
}

/* Fake side: what PassThruConnect was asked, before it judges it. */
inline void record_connect(Page* p, uint32_t protocol, uint32_t flags,
                           uint32_t baud)
{
    Guard g(p);
    p->connect_protocol = protocol;
    p->connect_flags = flags;
    p->connect_baud = baud;
}

/* Fake side: a filter it started (len at most kFilterMax). */
inline void record_filter(Page* p, uint32_t id, uint32_t type,
                          const uint8_t* mask, const uint8_t* pattern,
                          uint32_t len)
{
    Guard g(p);
    Filter& f = p->last_filter;
    f.id = id;
    f.type = type;
    f.len = (len < kFilterMax) ? len : kFilterMax;
    memset(f.mask, 0, kFilterMax);
    memset(f.pattern, 0, kFilterMax);
    memcpy(f.mask, mask, f.len);
    memcpy(f.pattern, pattern, f.len);
}

/* Fake side: a filter ID it was asked to stop. */
inline void record_filter_stop(Page* p, uint32_t id)
{
    Guard g(p);
    p->last_stopped = id;
}

inline uint32_t writes(Page* p)
{
    return static_cast<uint32_t>(InterlockedCompareExchange(&p->writes, 0, 0));
}

/* Write i since the reset; false (and w zeroed) past those kept. */
inline bool recorded_write(Page* p, uint32_t i, Write* w)
{
    Guard g(p);
    memset(w, 0, sizeof(*w));
    if (i >= kWriteDepth ||
        i >= static_cast<uint32_t>(InterlockedCompareExchange(&p->writes, 0, 0))) {
        return false;
    }
    *w = p->recorded[i];
    return true;
}

inline void last_connect(Page* p, uint32_t* protocol, uint32_t* flags,
                         uint32_t* baud)
{
    Guard g(p);
    *protocol = p->connect_protocol;
    *flags = p->connect_flags;
    *baud = p->connect_baud;
}

inline Filter last_filter(Page* p)
{
    Guard g(p);
    return p->last_filter;
}

inline uint32_t last_stopped(Page* p)
{
    Guard g(p);
    return p->last_stopped;
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
