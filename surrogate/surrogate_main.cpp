// File/layer: surrogate/surrogate_main.cpp | Win32, x86-only, Qt-free
// Testing: HARNESS — spawned by the client and by test_j2534_surrogate
// CLI contract: surrogate.exe <dll_path> <pipe_name> <map_name> <event_name>
//               (all names generated and owned by the x64 app side)
//
// Part 4: control pump gains WRITE / IOCTL / START_FILTER / STOP_FILTER
// relays. The flat wire forms (byte arrays, flattened SCONFIG pairs) are
// expanded HERE into real vendor-ABI structures — PASSTHRU_MSG and
// SCONFIG_LIST — in THIS process's address space, because that is the only
// place their pointers are meaningful. Nothing pointer-shaped ever crosses
// the pipe.

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
typedef long (__stdcall *fn_pt_write_msgs)(unsigned long, PASSTHRU_MSG*,
                                           unsigned long*, unsigned long);
typedef long (__stdcall *fn_pt_ioctl)(unsigned long, long,
                                      void*, void*);
typedef long (__stdcall *fn_pt_start_filter)(unsigned long, unsigned long,
                                              PASSTHRU_MSG*, PASSTHRU_MSG*,
                                              PASSTHRU_MSG*, unsigned long*);
typedef long (__stdcall *fn_pt_stop_filter)(unsigned long, unsigned long);

static fn_pt_open         g_open;
static fn_pt_close        g_close;
static fn_pt_connect      g_connect;
static fn_pt_disconnect   g_disconnect;
static fn_pt_read_msgs    g_read_msgs;
static fn_pt_write_msgs   g_write_msgs;
static fn_pt_ioctl        g_ioctl;
static fn_pt_start_filter g_start_filter;
static fn_pt_stop_filter  g_stop_filter;

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static HANDLE g_map = nullptr;
static qttune_sur_ring_t* g_ring = nullptr;
static HANDLE g_evt = nullptr;

static void push_slot(const PASSTHRU_MSG* msg)
{
    const uint32_t head = g_ring->head;
    if (head - g_ring->tail >= QTTUNE_SURROGATE_RING_SIZE) {
        return;   /* full: drop (backpressure counter = Part 4 work) */
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

/* Expand a flat (bytes, len) wire pattern into a vendor-ABI
 * PASSTHRU_MSG. Zeroed FIRST — Reserved fields / stale padding must
 * never reach a vendor comparing struct contents. */
static void fill_pattern_msg(PASSTHRU_MSG* msg, const uint8_t* bytes,
                             uint32_t len, uint32_t tx_flags)
{
    *msg = PASSTHRU_MSG{};
    msg->ProtocolID = QTTUNE_PT_PROTOCOL_ISO15765;
    msg->TxFlags = tx_flags;
    msg->DataSize = len;
    if (len > 0) {
        memcpy(msg->Data, bytes, len);
    }
    msg->ExtraDataIndex = len;   /* no extra data */
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
        case QTTUNE_SUR_OP_WRITE: {
            /* ONE message per request (wire limit 64 bytes; ISO-TP
             * multiframe beyond that is future transport work). */
            PASSTHRU_MSG msg;
            fill_pattern_msg(&msg, req.u.write.data, req.u.write.data_len,
                             req.u.write.tx_flags);
            unsigned long num = 1;
            resp.status = g_write_msgs(req.u.write.channel_id, &msg, &num,
                                       req.u.write.timeout_ms);
            resp.aux = num;   /* actual written — 0 or partial = caller's call */
            break;
        }
        case QTTUNE_SUR_OP_IOCTL: {
            const uint32_t id = req.u.ioctl.ioctl_id;
            if (id == QTTUNE_PT_IOCTL_GET_CONFIG ||
                id == QTTUNE_PT_IOCTL_SET_CONFIG) {
                /* Flattened (Parameter, Value) pairs -> real SCONFIG_LIST.
                 * input_len is validated by the client; re-validate here —
                 * the pipe is a trust boundary too. */
                const uint32_t n = req.u.ioctl.input_len / 8;
                if ((req.u.ioctl.input_len % 8) != 0 ||
                    n == 0 || n > QTTUNE_PT_SCONFIG_MAX_PARAMS) {
                    resp.status = QTTUNE_PT_STATUS_FAILED;
                    break;
                }
                SCONFIG cfg[QTTUNE_PT_SCONFIG_MAX_PARAMS];
                for (uint32_t i = 0; i < n; ++i) {
                    uint32_t param = 0, value = 0;
                    memcpy(&param, req.u.ioctl.input + i * 8, 4);
                    memcpy(&value, req.u.ioctl.input + i * 8 + 4, 4);
                    cfg[i].Parameter = param;
                    cfg[i].Value = value;
                }
                SCONFIG_LIST list = { n, cfg };
                resp.status = g_ioctl(req.u.ioctl.channel_id,
                                      static_cast<long>(id), &list, nullptr);
                /* GET_CONFIG writes results back through pInput —
                 * re-flatten for the response. */
                if (id == QTTUNE_PT_IOCTL_GET_CONFIG &&
                    resp.status == QTTUNE_PT_STATUS_NOERROR) {
                    for (uint32_t i = 0; i < n; ++i) {
                        memcpy(resp.data + i * 8, &cfg[i].Parameter, 4);
                        memcpy(resp.data + i * 8 + 4, &cfg[i].Value, 4);
                    }
                    resp.data_len = req.u.ioctl.input_len;
                }
            } else if (id == QTTUNE_PT_IOCTL_READ_VBATT) {
                unsigned long vbatt = 0;
                resp.status = g_ioctl(req.u.ioctl.channel_id,
                                      static_cast<long>(id), nullptr, &vbatt);
                if (resp.status == QTTUNE_PT_STATUS_NOERROR) {
                    memcpy(resp.data, &vbatt, 4);
                    resp.data_len = 4;
                }
            } else if (id == QTTUNE_PT_IOCTL_CLEAR_TX_BUFFER ||
                       id == QTTUNE_PT_IOCTL_CLEAR_RX_BUFFER ||
                       id == QTTUNE_PT_IOCTL_CLEAR_MSG_FILTERS) {
                resp.status = g_ioctl(req.u.ioctl.channel_id,
                                      static_cast<long>(id), nullptr, nullptr);
            } else {
                /* Out of contract: pointer-bearing ioctls must not be
                 * relayed with NULL payloads — refuse, matching the
                 * loader seam's whitelist (internal.h contract). */
                resp.status = QTTUNE_PT_STATUS_FAILED;
            }
            break;
        }
        case QTTUNE_SUR_OP_START_FILTER: {
            PASSTHRU_MSG mask, pattern, fc;
            fill_pattern_msg(&mask, req.u.start_filter.mask,
                             req.u.start_filter.mask_len,
                             req.u.start_filter.tx_flags);
            fill_pattern_msg(&pattern, req.u.start_filter.pattern,
                             req.u.start_filter.pattern_len,
                             req.u.start_filter.tx_flags);
            PASSTHRU_MSG* fc_ptr = nullptr;
            if (req.u.start_filter.fc_len > 0) {
                fill_pattern_msg(&fc, req.u.start_filter.flow_control,
                                 req.u.start_filter.fc_len,
                                 req.u.start_filter.tx_flags);
                fc_ptr = &fc;
            }
            unsigned long filter_id = 0;
            resp.status = g_start_filter(req.u.start_filter.channel_id,
                                         req.u.start_filter.filter_type,
                                         &mask, &pattern, fc_ptr, &filter_id);
            resp.handle_or_channel = filter_id;
            break;
        }
        case QTTUNE_SUR_OP_STOP_FILTER:
            resp.status = g_stop_filter(req.u.stop_filter.channel_id,
                                        req.u.stop_filter.filter_id);
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

    /* Vendor load + mandatory-export resolution (Part 4: the full nine
     * functions the pumps need — the loader's in-proc path resolves the
     * same set via its own table). */
    g_module = LoadLibraryA(dll_path);
    if (g_module == nullptr) return 4;
    g_open          = reinterpret_cast<fn_pt_open>(GetProcAddress(g_module, "PassThruOpen"));
    g_close         = reinterpret_cast<fn_pt_close>(GetProcAddress(g_module, "PassThruClose"));
    g_connect       = reinterpret_cast<fn_pt_connect>(GetProcAddress(g_module, "PassThruConnect"));
    g_disconnect    = reinterpret_cast<fn_pt_disconnect>(GetProcAddress(g_module, "PassThruDisconnect"));
    g_read_msgs     = reinterpret_cast<fn_pt_read_msgs>(GetProcAddress(g_module, "PassThruReadMsgs"));
    g_write_msgs    = reinterpret_cast<fn_pt_write_msgs>(GetProcAddress(g_module, "PassThruWriteMsgs"));
    g_ioctl         = reinterpret_cast<fn_pt_ioctl>(GetProcAddress(g_module, "PassThruIoCtl"));
    g_start_filter  = reinterpret_cast<fn_pt_start_filter>(GetProcAddress(g_module, "PassThruStartMsgFilter"));
    g_stop_filter   = reinterpret_cast<fn_pt_stop_filter>(GetProcAddress(g_module, "PassThruStopMsgFilter"));
    if (!g_open || !g_close || !g_connect || !g_disconnect || !g_read_msgs ||
        !g_write_msgs || !g_ioctl || !g_start_filter || !g_stop_filter) {
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
