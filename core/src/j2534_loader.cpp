// File/layer: core/src/j2534_loader.cpp | C++17, Win32-guarded
// Testing: HARNESS — live-registry discovery tests + test_j2534_load
//          (load/export/open/close) + test_j2534_stream (end-to-end)
// CI: non-Windows gets linking stubs only; core.cpp guards routing

#include "qttune/j2534.h"
#include "qttune/core.h"
#include "qttune/internal.h"
#include "qttune/j2534_surrogate.h"

#ifdef _WIN32

#include <windows.h>
#include "qttune/j2534_ptypes.h"

#include <vector>
#include <algorithm>

/* ================================================================
 * GLOBAL SCOPE — completes types the header forward-declares.
 * Order matters: typedefs -> handle struct. Nothing here may sit
 * inside an anonymous namespace; the header's typedef targets the
 * global tag name (C2027 trap, learned the hard way).
 * ================================================================ */

typedef long (__stdcall *fn_pt_open)(void* pName, unsigned long* pDeviceID);
typedef long (__stdcall *fn_pt_close)(unsigned long DeviceID);
typedef long (__stdcall *fn_pt_connect)(unsigned long DeviceID,
                                        unsigned long ProtocolID,
                                        unsigned long Flags,
                                        unsigned long BaudRate,
                                        unsigned long* pChannelID);
typedef long (__stdcall *fn_pt_disconnect)(unsigned long ChannelID);
typedef long (__stdcall *fn_pt_read_msgs)(unsigned long ChannelID,
                                          PASSTHRU_MSG* pMsg,
                                          unsigned long* pNumMsgs,
                                          unsigned long Timeout);
typedef long (__stdcall *fn_pt_write_msgs)(unsigned long ChannelID,
                                           PASSTHRU_MSG* pMsg,
                                           unsigned long* pNumMsgs,
                                           unsigned long Timeout);
typedef long (__stdcall *fn_pt_ioctl)(unsigned long ChannelID,
                                      long IoControlID,
                                      void* pInput,
                                      void* pOutput);

/* Backing type for the header's opaque qttune_j2534_handle_t.
 *
 * BACKEND SELECTION (Part 3 routing): qttune_j2534_open sniffs the PE
 * architecture once and routes — x86 DLLs load in the surrogate
 * process (an x64 process physically cannot LoadLibrary a 32-bit
 * DLL); x64/unknown load in-proc. Every downstream helper forwards
 * on is_surrogate, so the transport layer (j2534_transport.cpp) stays
 * single-path and unaware of which backend serves any given session. */
struct qttune_j2534_handle {
    bool is_surrogate = false;
    qttune_j2534_sur_handle_t* sur = nullptr;  /* owned iff is_surrogate */
    HMODULE module = nullptr;                  /* owned iff !is_surrogate */
    unsigned long device_id;
    fn_pt_open        open;
    fn_pt_close       close;
    fn_pt_connect     connect;
    fn_pt_disconnect  disconnect;
    fn_pt_read_msgs   read_msgs;
    fn_pt_write_msgs  write_msgs;
    fn_pt_ioctl       ioctl;
};

namespace {

constexpr long kPtStatusNoError = QTTUNE_PT_STATUS_NOERROR;

template <typename FnPtr>
FnPtr resolve_export(HMODULE module, const char* name, bool& all_ok)
{
    FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr) {
        all_ok = false;
        return nullptr;
    }
    return reinterpret_cast<FnPtr>(proc);
}

/* Root registry keys swept by discovery, per SAE J2534-1 v4.04 §5.3
 * ("Pass-Thru Support.04.04") PLUS the no-hyphen variant some vendor
 * installers write ("PassThruSupport.04.04" — observed: OBDX Pro).
 * Both spellings exist in the wild; the spec spelling is kept first
 * so conforming vendors keep discovery-order precedence. Dedupe in
 * qttune_j2534_discover_devices absorbs a vendor writing both. */
constexpr const wchar_t* kPassThruRoots[] = {
    L"SOFTWARE\\Pass-Thru Support.04.04",
    L"SOFTWARE\\PassThruSupport.04.04",
};

bool read_function_library(HKEY hvendor, char* out, size_t out_size)
{
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(out_size);
    LONG rc = RegQueryValueExA(hvendor, "FUNCTIONLIBRARY", nullptr,
                               &type, reinterpret_cast<LPBYTE>(out), &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    out[out_size - 1] = '\0';
    if (size > 0 && out[size - 1] != '\0' && size < out_size) {
        out[size] = '\0';
    }
    return out[0] != '\0';
}

/* Sniff PE header architecture from a DLL path.
 * Returns: 1=x86, 2=x64, 0=unknown — matching j2534.h's documented
 * reserved[0] contract (which this function's docstring previously
 * claimed but its code contradicted by defaulting failures to 1).
 *
 * ROUTING CONSUMER: qttune_j2534_open sends x86 to the surrogate and
 * takes everything else in-proc. 0 on unreadable/invalid PE therefore
 * preserves the pre-routing behavior: LoadLibraryEx fails honestly
 * with QT_ERR_LOAD_FAILED, no surrogate spawn for garbage paths.
 * The discovery annotation (reserved[0]) is a UI label; the sniff in
 * open() is authoritative and covers direct-path selectors that
 * bypass discovery entirely. */
static uint32_t sniff_pe_architecture(const char* dll_path)
{
    HANDLE hFile = CreateFileA(dll_path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return 0; // unknown — open() falls back to in-proc
    }

    DWORD bytes_read;
    IMAGE_DOS_HEADER dos_hdr = {};
    if (!ReadFile(hFile, &dos_hdr, sizeof(dos_hdr), &bytes_read, NULL) ||
        dos_hdr.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(hFile);
        return 0;
    }

    SetFilePointer(hFile, dos_hdr.e_lfanew, NULL, FILE_BEGIN);
    IMAGE_NT_HEADERS nt_hdr = {};
    if (!ReadFile(hFile, &nt_hdr, sizeof(nt_hdr), &bytes_read, NULL) ||
        nt_hdr.Signature != IMAGE_NT_SIGNATURE) {
        CloseHandle(hFile);
        return 0;
    }

    uint16_t machine = nt_hdr.FileHeader.Machine;
    CloseHandle(hFile);

    if (machine == IMAGE_FILE_MACHINE_I386) {
        return 1; // x86 — surrogate required from an x64 host process
    } else if (machine == IMAGE_FILE_MACHINE_AMD64) {
        return 2; // x64 — in-proc
    } else {
        return 0; // ARM64/Itanium/etc — unknown, in-proc attempt
    }
}

void enumerate_view(const wchar_t* root_path,
                    REGSAM wow64_flag,
                    std::vector<qttune_j2534_device_t>& out_devices)
{
    HKEY hroot = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, root_path, 0,
                            KEY_READ | wow64_flag, &hroot);
    if (rc != ERROR_SUCCESS || hroot == nullptr) {
        return;
    }

    DWORD subkey_count = 0;
    RegQueryInfoKeyW(hroot, nullptr, nullptr, nullptr, &subkey_count,
                     nullptr, nullptr, nullptr, nullptr, nullptr,
                     nullptr, nullptr);

    for (DWORD i = 0; i < subkey_count; ++i) {
        if (out_devices.size() >= QTTUNE_J2534_MAX_VENDORS) {
            break;
        }

        wchar_t subkey_name[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        if (RegEnumKeyExW(hroot, i, subkey_name, &len, nullptr,
                          nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            continue;
        }

        HKEY hvendor = nullptr;
        if (RegOpenKeyExW(hroot, subkey_name, 0, KEY_READ | wow64_flag,
                          &hvendor) != ERROR_SUCCESS || hvendor == nullptr) {
            continue;
        }

        qttune_j2534_device_t dev = {};
        dev.vendor_id = static_cast<uint32_t>(out_devices.size());

        WideCharToMultiByte(CP_UTF8, 0, subkey_name, -1,
                            dev.vendor_name, QTTUNE_J2534_VENDOR_NAME_LEN,
                            nullptr, nullptr);

        if (read_function_library(hvendor, dev.dll_path,
                                  sizeof(dev.dll_path))) {
            /* Annotate architecture (reserved[0]: 1=x86, 2=x64, per
             * j2534.h) — feeds the picker's x86/x64 labeling and,
             * later, the surrogate-vs-inproc routing decision. Cheap
             * file-I/O per discovered vendor (typically 1-3 entries);
             * the file was just validated as openable-by-name anyway. */
            dev.reserved[0] = sniff_pe_architecture(dev.dll_path);                       
            out_devices.push_back(dev);
        }
        RegCloseKey(hvendor);
    }

    RegCloseKey(hroot);
}

} // namespace

extern "C" {

qttune_status_t qttune_j2534_discover_devices(qttune_j2534_device_t** devices_out,
                                              uint32_t* device_count)
{
    if (devices_out == nullptr || device_count == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }

    *devices_out = nullptr;
    *device_count = 0;

    std::vector<qttune_j2534_device_t> devices;
    devices.reserve(4);

    /* Both root spellings x both WOW64 views = 4 sweeps. Identical
     * entries across sweeps collapse in the dedupe below (dll_path +
     * vendor name); a vendor that wrote only the compacted root under
     * the 32-bit view (the OBDX Pro case) is found on exactly one
     * sweep and passes straight through. */
    for (const wchar_t* root : kPassThruRoots) {
        enumerate_view(root, KEY_WOW64_64KEY, devices);
        enumerate_view(root, KEY_WOW64_32KEY, devices);
    }

    std::vector<qttune_j2534_device_t> unique;
    for (const auto& d : devices) {
        bool dup = false;
        for (const auto& u : unique) {
            if (_stricmp(d.dll_path, u.dll_path) == 0 &&
                strcmp(d.vendor_name, u.vendor_name) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            unique.push_back(d);
        }
    }
    for (auto& d : unique) {
        d.vendor_id = static_cast<uint32_t>(&d - &unique[0]);
    }

    if (unique.empty()) {
        return QT_OK;
    }

    try {
        qttune_j2534_device_t* copy = new qttune_j2534_device_t[unique.size()];
        std::copy(unique.begin(), unique.end(), copy);
        *devices_out = copy;
        *device_count = static_cast<uint32_t>(unique.size());
        return QT_OK;
    } catch (...) {
        return QT_ERR_OUT_OF_MEMORY;
    }
}

void qttune_j2534_free_devices(qttune_j2534_device_t* devices)
{
    delete[] devices;
}

qttune_status_t qttune_j2534_open(const qttune_j2534_device_t* device,
                                  qttune_j2534_handle_t** out_handle)
{
    if (device == nullptr || out_handle == nullptr ||
        device->dll_path[0] == '\0') {
        return QT_ERR_NULL_ARGUMENT;
    }
    *out_handle = nullptr;

    /* ROUTING DECISION — exactly one place, exactly once. x86 DLLs
     * delegate to the surrogate host (exe lookup honors
     * QTTUNE_SURROGATE_EXE); everything else proceeds in-proc below,
     * including unknown (sniff 0), so unreadable paths still fail at
     * LoadLibraryEx with today's ergonomics. */
    if (sniff_pe_architecture(device->dll_path) == 1) {
        qttune_j2534_sur_handle_t* sur = nullptr;
        qttune_status_t st = qttune_j2534_sur_open(device->dll_path, &sur);
        if (st != QT_OK) {
            return st;   /* spawn/DLL-load/PassThruOpen failure — already
                            mapped to QT_ERR_LOAD_FAILED client-side */
        }
        qttune_j2534_handle_t* h = new (std::nothrow) qttune_j2534_handle_t();
        if (h == nullptr) {
            qttune_j2534_sur_close(sur);
            return QT_ERR_OUT_OF_MEMORY;
        }
        h->is_surrogate = true;
        h->sur = sur;
        *out_handle = h;
        return QT_OK;
    }

    HMODULE module = LoadLibraryExA(device->dll_path, nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        return QT_ERR_LOAD_FAILED;
    }

    qttune_j2534_handle_t* h = new (std::nothrow) qttune_j2534_handle_t();
    if (h == nullptr) {
        FreeLibrary(module);
        return QT_ERR_OUT_OF_MEMORY;
    }
    h->module = module;

    bool all_resolved = true;
    h->open       = resolve_export<fn_pt_open>(module, "PassThruOpen", all_resolved);
    h->close      = resolve_export<fn_pt_close>(module, "PassThruClose", all_resolved);
    h->connect    = resolve_export<fn_pt_connect>(module, "PassThruConnect", all_resolved);
    h->disconnect = resolve_export<fn_pt_disconnect>(module, "PassThruDisconnect", all_resolved);
    h->read_msgs  = resolve_export<fn_pt_read_msgs>(module, "PassThruReadMsgs", all_resolved);
    h->write_msgs = resolve_export<fn_pt_write_msgs>(module, "PassThruWriteMsgs", all_resolved);
    h->ioctl      = resolve_export<fn_pt_ioctl>(module, "PassThruIoCtl", all_resolved);

    if (!all_resolved) {
        FreeLibrary(module);
        delete h;
        return QT_ERR_LOAD_FAILED;
    }

    unsigned long device_id = 0;
    if (h->open(nullptr, &device_id) != kPtStatusNoError) {
        FreeLibrary(module);
        delete h;
        return QT_ERR_LOAD_FAILED;
    }
    h->device_id = device_id;

    *out_handle = h;
    return QT_OK;
}

qttune_status_t qttune_j2534_close(qttune_j2534_handle_t* handle)
{
    if (handle == nullptr) {
        return QT_OK;
    }
    if (handle->is_surrogate) {
        /* Polite SHUTDOWN, bounded wait, job-object kill — all inside
         * the surrogate client. Deleting the box after is safe: the
         * transport already joined its worker before calling here. */
        qttune_status_t st = qttune_j2534_sur_close(handle->sur);
        delete handle;
        return st;
    }
    if (handle->close != nullptr) {
        handle->close(handle->device_id);
    }
    if (handle->module != nullptr) {
        FreeLibrary(handle->module);
    }
    delete handle;
    return QT_OK;
}

/* ---- Vendor-call helpers used by j2534_transport.cpp ---- */

qttune_status_t qttune_j2534_connect_channel(qttune_j2534_handle_t* handle,
                                             unsigned long baud,
                                             unsigned long flags,
                                             unsigned long* out_channel)
{
    if (handle == nullptr || out_channel == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (handle->is_surrogate) {
        return qttune_j2534_sur_connect_channel(handle->sur, baud,
                                                 flags, out_channel);
    }
    /* baud == 0 is the transport-layer "unset" sentinel (the URI parser
     * rejects an explicit baud=0, so the ambiguity is closed by
     * contract); resolve it to the ISO 15765 default. flags pass
     * through verbatim — 0 is both the default and a legitimate value. */
    if (baud == 0) {
        baud = QTTUNE_PT_BAUD_ISO15765_500K;
    }
    long rc = handle->connect(handle->device_id,
                              QTTUNE_PT_PROTOCOL_ISO15765,
                              flags,
                              baud,
                              out_channel);
    return (rc == kPtStatusNoError) ? QT_OK : QT_ERR_NO_TRANSPORT;
}

qttune_status_t qttune_j2534_disconnect_channel(qttune_j2534_handle_t* handle,
                                                unsigned long channel)
{
    if (handle == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (handle->is_surrogate) {
        return qttune_j2534_sur_disconnect_channel(handle->sur, channel);
    }
    /* Best-effort at teardown: vendor failure during shutdown must
     * not block the detach path. */
    long rc = handle->disconnect(channel);
    return (rc == kPtStatusNoError) ? QT_OK : QT_OK;
}

qttune_status_t qttune_j2534_read_frame(qttune_j2534_handle_t* handle,
                                        unsigned long channel,
                                        qttune_frame_t* out_frame,
                                        uint32_t timeout_ms,
                                        bool* got_frame)
{
    if (handle == nullptr || out_frame == nullptr || got_frame == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    *got_frame = false;

    if (handle->is_surrogate) {
        return qttune_j2534_sur_read_frame(handle->sur, channel,
                                           out_frame, timeout_ms,
                                           got_frame);
    }

    PASSTHRU_MSG msg = {};
    unsigned long num = 1;
    long rc = handle->read_msgs(channel, &msg, &num, timeout_ms);

    /* NOTE: any nonzero vendor status (including idle-timeout on a
     * real device) is uniformly "no frame" in v0.3.0. Distinguishing
     * ERR_TIMEOUT from hard device errors is BENCH-tier work; the
     * fake never returns nonzero here. */
    if (rc != kPtStatusNoError || num == 0) {
        return QT_OK;
    }

    /* Wipe the whole struct first: the F1 ABI documents reserved as
     * must-be-zero and flags as a full word — a caller reusing one
     * frame variable across reads must never inherit stale bits. */
    *out_frame = qttune_frame_t{};

    /* J2534-1 PASSTHRU_MSG carries NO arbitration ID — CAN identity
     * is expressed via PassThruStartMsgFilter patterns, and ISO 15765
     * addressing bytes ride in the payload. Until the transport layer
     * learns the filter API (and what pattern a real vehicle needs,
     * BENCH-tier), the J2534 path reports id 0 and the bridge's
     * message-ID gate therefore does not match these frames. That is
     * CORRECT for v0.3.0: the mock set (0x0C0) is mock-only by
     * design; a real signal set for this path arrives with the filter
     * work. Documented, not silent. */
    out_frame->arbitration_id = 0;
    out_frame->channel_id = 0;
    out_frame->flags = (msg.RxStatus & QTTUNE_PT_CAN_29BIT_ID)
                        ? QTTUNE_FRAME_FLAG_EXTENDED : 0u;

    /* Units are vendor-defined (see j2534_ptypes.h) — passed through
     * untranslated per the documented v0.3.0 decision; normalization
     * is BENCH work once a real device reveals the vendor epoch. */
    out_frame->timestamp_us = msg.Timestamp;

    /* Clamp before copy: a buggy vendor DATA_LENGTH is the documented
     * overflow vector (also flagged high-risk in the surrogate design).
     * data_length is uint16_t in the F1 ABI — cast follows the clamp. */
    unsigned long n = msg.DataSize;
    if (n > sizeof(out_frame->data)) {
        n = sizeof(out_frame->data);
    }
    out_frame->data_length = static_cast<uint16_t>(n);
    memcpy(out_frame->data, msg.Data, n);

    *got_frame = true;
    return QT_OK;
}

const char* qttune_j2534_backend_name(const qttune_j2534_handle_t* handle)
{
    if (handle == nullptr) {
        return nullptr;
    }
    return handle->is_surrogate ? "surrogate" : "inproc";
}

/* TEST-ONLY: forward an injected frame into the loaded module's
 * InjectFrame hook (fake DLL). Routes by backend like everything
 * else: surrogate mode crosses the control pipe into the child's
 * module instance; in-proc mode resolves InjectFrame lazily — it is
 * a test hook, not a J2534 mandatory export, so a DLL without it
 * yields QT_ERR_NOT_IMPLEMENTED rather than an open-time failure. */
qttune_status_t qttune_j2534_inject_test_frame(qttune_j2534_handle_t* handle,
                                               const uint8_t* data,
                                               uint32_t len,
                                               uint32_t timestamp,
                                               uint32_t rx_flags)
{
    if (handle == nullptr || data == nullptr || len == 0 || len > 64) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (handle->is_surrogate) {
        return qttune_j2534_sur_inject_test_frame(handle->sur, data, len,
                                                  timestamp, rx_flags);
    }
    if (handle->module == nullptr) {
        return QT_ERR_NO_TRANSPORT;
    }
    typedef void (*fn_inject)(const unsigned char*, unsigned long,
                              unsigned long, unsigned long, unsigned long);
    fn_inject inject = reinterpret_cast<fn_inject>(
        GetProcAddress(handle->module, "InjectFrame"));
    if (inject == nullptr) {
        return QT_ERR_NOT_IMPLEMENTED;
    }
    inject(data, len, timestamp, rx_flags, 0);
    return QT_OK;
}

} // extern "C"

#else

extern "C" {

qttune_status_t qttune_j2534_discover_devices(qttune_j2534_device_t** devices_out,
                                              uint32_t* device_count)
{
    if (devices_out == nullptr || device_count == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    *devices_out = nullptr;
    *device_count = 0;
    return QT_ERR_NOT_IMPLEMENTED;
}

void qttune_j2534_free_devices(qttune_j2534_device_t* /*devices*/)
{
}

qttune_status_t qttune_j2534_open(const qttune_j2534_device_t* /*device*/,
                                  qttune_j2534_handle_t** out_handle)
{
    if (out_handle != nullptr) {
        *out_handle = nullptr;
    }
    return QT_ERR_NOT_IMPLEMENTED;
}

qttune_status_t qttune_j2534_close(qttune_j2534_handle_t* /*handle*/)
{
    return QT_ERR_NOT_IMPLEMENTED;
}

qttune_status_t qttune_j2534_connect_channel(qttune_j2534_handle_t* /*h*/,
                                             unsigned long /*baud*/,
                                             unsigned long /*flags*/,
                                             unsigned long* /*ch*/)
{
    return QT_ERR_NOT_IMPLEMENTED;
}

qttune_status_t qttune_j2534_disconnect_channel(qttune_j2534_handle_t* /*h*/,
                                                unsigned long /*ch*/)
{
    return QT_ERR_NOT_IMPLEMENTED;
}

qttune_status_t qttune_j2534_read_frame(qttune_j2534_handle_t* /*h*/,
                                        unsigned long /*ch*/,
                                        qttune_frame_t* /*f*/,
                                        uint32_t /*t*/,
                                        bool* got_frame)
{
    if (got_frame != nullptr) {
        *got_frame = false;
    }
    return QT_ERR_NOT_IMPLEMENTED;
}

const char* qttune_j2534_backend_name(const qttune_j2534_handle_t* /*handle*/)
{
    return nullptr;
}

qttune_status_t qttune_j2534_inject_test_frame(qttune_j2534_handle_t* /*h*/,
                                              const uint8_t* /*d*/,
                                              uint32_t /*len*/,
                                              uint32_t /*ts*/,
                                              uint32_t /*flags*/)
{
    return QT_ERR_NOT_IMPLEMENTED;
}

} // extern "C"

#endif /* _WIN32 */
