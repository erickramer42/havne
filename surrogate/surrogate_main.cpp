// File/layer: surrogate/surrogate_main.cpp | Win32, x86-only, Qt-free
// Testing: HARNESS — spawned by the client and by test_j2534_surrogate
// CLI contract: surrogate.exe <dll_path> <pipe_name> <map_name> <event_name>
//               (all names generated and owned by the x64 app side)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "qttune/j2534.h"
#include "qttune/j2534_ptypes.h"
#include "qttune/surrogate_protocol.h"

#include <cstring>

static HMODULE g_module = nullptr;
static unsigned long g_device_id = 0;
static unsigned long g_channel = 0;         /* set by CONNECT, read by pump */
static volatile LONG g_stop = 0;

typedef long (__stdcall *fn_pt_open)(void*, unsigned long*);
typedef long (__stdcall *fn_pt_close)(unsigned long);
typedef long (__stdcall *fn_pt_connect)(unsigned long, unsigned long, unsigned long,
                                        unsigned long, unsigned long*);
typedef long (__stdcall *fn_pt_disconnect)(unsigned long);
typedef long (__stdcall *fn_pt_read_msgs)(unsigned long, PASSTHRU_MSG*,
                                          unsigned long*, unsigned long);

static fn_pt_open        g_open;
static fn_pt_close       g_close;
static fn_pt_connect     g_connect;
static fn_pt_disconnect  g_disconnect;
static fn_pt_read_msgs   g_read_msgs;

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static HANDLE g_map = nullptr;
static qttune_sur_ring_t* g_ring = nullptr;
static HANDLE g_evt = nullptr;

static void push_slot(const PASSTHRU_MSG* msg)
{
    const uint32_t head = g_ring->head;
    if (head - g_ring->tail >= QTTUNE_SURROGATE_RING_SIZE) {
        return;   /* full: drop (backpressure counter = Part 3) */
    }
    qttune_sur_ring_slot_t* s =
        &g_ring->slots[head & (QTTUNE_SURROGATE_RING_SIZE - 1)];
    unsigned long n = msg->DataSize;
    if (n > sizeof(s->data)) n = sizeof(s->data);
    memcpy(s->data, msg->Data, n);
    s->data_len = n;
    s->protocol_id = msg->ProtocolID;
    s->rx_status = msg->RxStatus;
    s->timestamp = msg->Timestamp;
    s->extra_data_index = msg->ExtraDataIndex;
    g_ring->head = head + 1;               /* publish AFTER fill */
    SetEvent(g_evt);
}

/* Data plane: vendor ReadMsgs with timeout 0 (non-blocking per
 * J2534-1), short sleep when quiet. Vendor variance in timeout-0
 * semantics is a known swamp — BENCH round arbitrates per-vendor. */
static DWORD WINAPI read_pump(LPVOID)
{
    for (;;) {
        if (g_stop) return 0;
        const unsigned long ch = g_channel;
        if (ch == 0 || g_read_msgs == nullptr) { Sleep(20); continue; }
        PASSTHRU_MSG msg = {};
        unsigned long num = 1;
        long rc = g_read_msgs(ch, &msg, &num, 0);
        if (rc != QTTUNE_PT_STATUS_NOERROR || num == 0) { Sleep(2); continue; }
        push_slot(&msg);
    }
}

static DWORD WINAPI control_pump(LPVOID)
{
    for (;;) {
        qttune_sur_request_t req = {};
        DWORD rd = 0;
        if (!ReadFile(g_pipe, &req, sizeof(req), &rd, nullptr) ||
            rd != sizeof(req)) {
            break;                          /* broken pipe: parent gone */
        }

        qttune_sur_response_t resp = {};
        resp.request_id = req.request_id;
        resp.status = QTTUNE_PT_STATUS_FAILED;
        bool quit = false;

        switch (req.opcode) {
        case QTTUNE_SUR_OP_OPEN:
            resp.status = QTTUNE_PT_STATUS_NOERROR;
            resp.handle_or_channel = g_device_id;
            break;
        case QTTUNE_SUR_OP_CONNECT: {
            unsigned long ch = 0;
            resp.status = g_connect(g_device_id, req.u.connect.protocol_id,
                                    req.u.connect.flags, req.u.connect.baud, &ch);
            resp.handle_or_channel = ch;
            if (resp.status == QTTUNE_PT_STATUS_NOERROR) {
                InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_channel),
                                    static_cast<LONG>(ch));
            }
            break;
        }
        case QTTUNE_SUR_OP_DISCONNECT:
            resp.status = g_disconnect(req.u.disconnect.channel_id);
            break;
        case QTTUNE_SUR_OP_INJECT: {
            /* TEST-ONLY: forward into the loaded module's hook. */
            typedef void (*fn_inject)(const unsigned char*, unsigned long,
                                      unsigned long, unsigned long, unsigned long);
            fn_inject inject = reinterpret_cast<fn_inject>(
                GetProcAddress(g_module, "InjectFrame"));
            if (inject != nullptr) {
                inject(req.u.inject.data, req.u.inject.len,
                       req.u.inject.timestamp, req.u.inject.rx_flags, 0);
                resp.status = QTTUNE_PT_STATUS_NOERROR;
            }
            break;
        }
        case QTTUNE_SUR_OP_SHUTDOWN:
            resp.status = QTTUNE_PT_STATUS_NOERROR;
            quit = true;
            break;
        default:
            break;
        }

        DWORD wr = 0;
        WriteFile(g_pipe, &resp, sizeof(resp), &wr, nullptr);
        FlushFileBuffers(g_pipe);
        if (quit) {
            InterlockedExchange(&g_stop, 1);
            break;
        }
    }
    InterlockedExchange(&g_stop, 1);
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc < 5) return 1;
    const char* dll_path = argv[1];
    const char* pipe_name = argv[2];
    const char* map_name = argv[3];
    const char* evt_name = argv[4];

    /* Attach to app-owned named objects. */
    g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, map_name);
    if (g_map == nullptr) return 2;
    g_ring = static_cast<qttune_sur_ring_t*>(
        MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    g_evt = OpenEventA(EVENT_MODIFY_STATE, FALSE, evt_name);
    if (g_ring == nullptr || g_evt == nullptr) return 3;

    /* Vendor load + mandatory-export resolution (subset the pumps need;
     * full 7-export check happens for the in-proc path — the surrogate
     * adds the remaining two when WRITE/IOCTL land in Part 3). */
    g_module = LoadLibraryA(dll_path);
    if (g_module == nullptr) return 4;
    g_open       = reinterpret_cast<fn_pt_open>(GetProcAddress(g_module, "PassThruOpen"));
    g_close      = reinterpret_cast<fn_pt_close>(GetProcAddress(g_module, "PassThruClose"));
    g_connect    = reinterpret_cast<fn_pt_connect>(GetProcAddress(g_module, "PassThruConnect"));
    g_disconnect = reinterpret_cast<fn_pt_disconnect>(GetProcAddress(g_module, "PassThruDisconnect"));
    g_read_msgs  = reinterpret_cast<fn_pt_read_msgs>(GetProcAddress(g_module, "PassThruReadMsgs"));
        if (!g_open || !g_close || !g_connect || !g_disconnect || !g_read_msgs) {
        return 5;
    }

    /* Vendor open happens HERE, before the pipe handshake — if the
     * DLL refuses, we exit, the app's ConnectNamedPipe (or the
     * process-alive check after it) fails, and the app reports
     * QT_ERR_LOAD_FAILED. Same failure-point ergonomics as in-proc. */
    long rc = g_open(nullptr, &g_device_id);
    if (rc != QTTUNE_PT_STATUS_NOERROR) {
        return 6;
    }

    /* WE are the pipe CLIENT — the app created the pipe and is
     * blocked in ConnectNamedPipe waiting for us. Bounded wait so a
     * dead parent can't strand us here (belt-and-braces; the job
     * object would kill us anyway on parent death). */
    for (int i = 0; i < 200; ++i) {
        if (WaitNamedPipeA(pipe_name, 50)) break;
    }
    g_pipe = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        g_close(g_device_id);
        return 7;
    }

    /* Byte-mode pipe needs message-boundary sync: one byte-mode write
     * per ReadFile pair on both sides (client writes then flushes;
     * we respond then flush — mirroring the app's transact()). */

    HANDLE ctrl = CreateThread(nullptr, 0, control_pump, nullptr, 0, nullptr);
    HANDLE rd   = CreateThread(nullptr, 0, read_pump, nullptr, 0, nullptr);
    if (ctrl == nullptr || rd == nullptr) {
        InterlockedExchange(&g_stop, 1);
    }

    /* Park until control pump exits (SHUTDOWN or broken pipe). The
     * read pump observes g_stop and exits on its next poll tick
     * (< 20 ms latency, irrelevant during teardown). */
    if (ctrl != nullptr) {
        WaitForSingleObject(ctrl, INFINITE);
        CloseHandle(ctrl);
    }
    if (rd != nullptr) {
        WaitForSingleObject(rd, 2000);   /* bounded: Sleep(20) worst case */
        CloseHandle(rd);
    }

    /* Teardown order — mirrors the in-proc transport contract
     * (worker quiesce BEFORE vendor close/unload). Channel first,
     * best-effort. */
    const unsigned long ch = g_channel;
    if (ch != 0) {
        g_disconnect(ch);
    }
    g_close(g_device_id);
    FreeLibrary(g_module);

    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    UnmapViewOfFile(g_ring);
    CloseHandle(g_map);
    CloseHandle(g_evt);
    return 0;
}
