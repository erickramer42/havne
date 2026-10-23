// File/layer: core/src/j2534_loader.cpp | C++17, Qt-free, CI-validated headless
// Testing: HARNESS — live-registry discovery tests, test_j2534_load
//          (load/export/open/close), test_j2534_stream (end to end) and
//          test_j2534_routing (backend choice and parity)
// CI: non-Windows gets linking stubs only; core.cpp guards routing

#include "ecuio/j2534.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/j2534_surrogate.h"

#ifdef _WIN32

#include <windows.h>
#include "ecuio/j2534_ptypes.h"
#include "error_detail.h"
#include "win_utf8.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

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
typedef long (__stdcall *fn_pt_start_filter)(unsigned long ChannelID,
                                             unsigned long FilterType,
                                             PASSTHRU_MSG* pMaskMsg,
                                             PASSTHRU_MSG* pPatternMsg,
                                             PASSTHRU_MSG* pFlowControlMsg,
                                             unsigned long* pFilterID);
typedef long (__stdcall *fn_pt_stop_filter)(unsigned long ChannelID,
                                             unsigned long FilterID);
typedef long (__stdcall *fn_pt_get_last_error)(char* pErrorDescription);
/* J2534-1 v04.04 (Quantex's PassThruReadVersion reference; the j2534-rs
 * binding): three caller buffers of at least 80 characters. */
typedef long (__stdcall *fn_pt_read_version)(unsigned long DeviceID,
                                             char* pFirmwareVersion,
                                             char* pDllVersion,
                                             char* pApiVersion);

/* Backing type for the header's opaque ecuio_j2534_handle_t.
 *
 * BACKEND SELECTION (ADR-0009): ecuio_j2534_open sniffs the PE
 * architecture once and routes — x86 DLLs load in the surrogate
 * process (an x64 process physically cannot LoadLibrary a 32-bit
 * DLL); x64/unknown load in-proc. Every downstream helper forwards
 * on is_surrogate, so the transport layer (j2534_transport.cpp) stays
 * single-path and unaware of which backend serves any given session. */
struct ecuio_j2534_handle {
    bool is_surrogate = false;
    uint32_t dll_arch = ECUIO_DLL_ARCH_UNKNOWN;   /* the PE sniff at open */
    ecuio_j2534_sur_handle_t* sur = nullptr;   /* owned iff is_surrogate */
    HMODULE module = nullptr;                  /* owned iff !is_surrogate */
    unsigned long device_id;
    fn_pt_open         open;
    fn_pt_close        close;
    fn_pt_connect      connect;
    fn_pt_disconnect   disconnect;
    fn_pt_read_msgs    read_msgs;
    fn_pt_write_msgs   write_msgs;
    fn_pt_ioctl        ioctl;
    fn_pt_start_filter start_filter;   /* mandatory in v04.04, like the rest */
    fn_pt_stop_filter  stop_filter;
    fn_pt_get_last_error get_last_error;   /* optional: may be null */
    fn_pt_read_version read_version;        /* optional: may be null */
};

namespace {

constexpr long kPtStatusNoError = ECUIO_PT_STATUS_NOERROR;

/* ecuio_j2534_force_surrogate_for_testing's switch. */
std::atomic<bool> g_force_surrogate{false};

/* The vendor's description of its last failed call
 * (PassThruGetLastError) into out, ECUIO_PT_TEXT_LEN + 1 bytes; "" when
 * it exports none or the call fails. The surrogate child relays the same
 * call (OP_GET_LAST_ERROR). */
void vendor_text(const ecuio_j2534_handle* h, char* out)
{
    out[0] = '\0';
    if (h->get_last_error == nullptr) {
        return;
    }
    char text[256] = {};   /* room past the spec's 80 for an overrun */
    if (h->get_last_error(text) != kPtStatusNoError) {
        return;
    }
    text[ECUIO_PT_TEXT_LEN] = '\0';
    memcpy(out, text, ECUIO_PT_TEXT_LEN + 1);
}

/* The first cap bytes of an ioctl result into output: the same cut on
 * both backends (the surrogate client applies it too). */
void copy_out(uint8_t* output, uint32_t cap, const void* src, uint32_t len)
{
    if (output != nullptr && len > 0) {
        memcpy(output, src, (len < cap) ? len : cap);
    }
}

/* Milliseconds from now to deadline, 0 once it has passed. */
uint32_t ms_left(std::chrono::steady_clock::time_point deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count());
}

/* Resolve by export name, with an optional alternate spelling.
 * GetProcAddress is case-sensitive: the spec spells it
 * 'PassThruIoctl' (SAE J2534-1_0500 §7.3.19, and the OBDX Pro VX
 * exports that), while this project once used 'PassThruIoCtl'. Spec
 * name first, legacy spelling second. Decorated or mangled names stay
 * hard failures. */
template <typename FnPtr>
FnPtr resolve_export(HMODULE module, const char* name, bool& all_ok,
                     const char* alt_name = nullptr)
{
    FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr && alt_name != nullptr) {
        proc = GetProcAddress(module, alt_name);
    }
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
 * ecuio_j2534_discover_devices absorbs a vendor writing both. */
constexpr const wchar_t* kPassThruRoots[] = {
    L"SOFTWARE\\Pass-Thru Support.04.04",
    L"SOFTWARE\\PassThruSupport.04.04",
};

/* FUNCTIONLIBRARY as a UTF-8 path (the j2534.h contract). Read as
 * UTF-16: the A variant would hand back the ANSI code page's bytes,
 * which the bridge and the loader read as UTF-8, so a non-ASCII install
 * path would mangle. */
bool read_function_library(HKEY hvendor, char* out, size_t out_size)
{
    wchar_t raw[ECUIO_J2534_DLL_PATH_LEN] = {};
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(sizeof(raw) - sizeof(wchar_t));   /* room
                                                    for a terminator */
    LONG rc = RegQueryValueExW(hvendor, L"FUNCTIONLIBRARY", nullptr,
                               &type, reinterpret_cast<LPBYTE>(raw), &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    /* The stored string need not be terminated. size is in bytes. */
    raw[size / sizeof(wchar_t)] = L'\0';
    if (raw[0] == L'\0') {
        return false;
    }
    /* RegQueryValueExW returns REG_EXPAND_SZ unexpanded, so a path
     * like "%ProgramFiles(x86)%\\..." would pass discovery and then
     * fail to load. ExpandEnvironmentStringsW's return counts the
     * terminator. */
    const wchar_t* path = raw;
    wchar_t expanded[1024];
    if (type == REG_EXPAND_SZ) {
        const DWORD n = ExpandEnvironmentStringsW(raw, expanded,
                                                   static_cast<DWORD>(_countof(expanded)));
        if (n == 0 || n > _countof(expanded)) {
            return false;   /* expansion failed or result won't fit */
        }
        path = expanded;
    }
    /* A path whose UTF-8 form won't fit the device struct is skipped,
     * not truncated: a truncated path loads the wrong file or none. */
    return ecuio_wide_to_utf8(path, out, out_size) && out[0] != '\0';
}

/* Sniff the PE architecture of a DLL: 1 = x86, 2 = x64, 0 = unknown
 * (j2534.h's reserved[0] contract).
 *
 * ecuio_j2534_open sends x86 to the surrogate and everything else
 * in-proc, so an unreadable or invalid file fails honestly at
 * LoadLibraryEx (ECUIO_ERR_LOAD_FAILED) without spawning a surrogate.
 * The discovery annotation is only a UI label; the sniff in open() is
 * authoritative and also covers direct-path selectors. */
static uint32_t sniff_pe_architecture(const char* dll_path)
{
    wchar_t wpath[ECUIO_J2534_DLL_PATH_LEN];
    if (!ecuio_utf8_to_wide(dll_path, wpath, _countof(wpath))) {
        return 0; // not UTF-8 — open() fails it at the in-proc load
    }
    HANDLE hFile = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ,
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
                    std::vector<ecuio_j2534_device_t>& out_devices)
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
        if (out_devices.size() >= ECUIO_J2534_MAX_VENDORS) {
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

        ecuio_j2534_device_t dev = {};
        dev.vendor_id = static_cast<uint32_t>(out_devices.size());

        /* A name too long for the struct stays empty rather than
         * unterminated; the device is still listed by its path. */
        (void)ecuio_wide_to_utf8(subkey_name, dev.vendor_name,
                                 sizeof(dev.vendor_name));

        if (read_function_library(hvendor, dev.dll_path,
                                  sizeof(dev.dll_path))) {
            /* Annotate architecture (reserved[0]) for the picker's
             * x86/x64 label. Routing re-sniffs at open. One small
             * file read per vendor (typically 1-3 entries). */
            dev.reserved[0] = sniff_pe_architecture(dev.dll_path);
            out_devices.push_back(dev);
        }
        RegCloseKey(hvendor);
    }

    RegCloseKey(hroot);
}

/* Build a PASSTHRU_MSG from flat bytes for the write and filter
 * relays. Zeroed first, so no stale field or padding byte reaches a
 * vendor comparing struct contents. ExtraDataIndex = DataSize (the
 * v4.04 no-extra-data convention); the surrogate child builds its
 * messages the same way. */
static void fill_vendor_msg(PASSTHRU_MSG* msg, const uint8_t* bytes,
                            uint32_t len, uint32_t tx_flags)
{
    *msg = PASSTHRU_MSG{};
    msg->ProtocolID = ECUIO_PT_PROTOCOL_ISO15765;
    msg->TxFlags = tx_flags;
    msg->DataSize = len;
    if (len > 0 && bytes != nullptr) {
        memcpy(msg->Data, bytes, len);
    }
    msg->ExtraDataIndex = len;
}

/* Unflatten (Parameter, Value) u32 LE pairs from the internal-seam
 * representation into a caller-supplied SCONFIG array. Shared by the
 * in-proc GET/SET_CONFIG path. Returns param count, or 0 on a
 * malformed length. */
static uint32_t unflatten_sconfig(const uint8_t* input, uint32_t input_len,
                                  SCONFIG* cfg, uint32_t max_params)
{
    if (input == nullptr || input_len == 0 ||
        (input_len % 8) != 0) {
        return 0;
    }
    const uint32_t n = input_len / 8;
    if (n > max_params) {
        return 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t param = 0, value = 0;
        memcpy(&param, input + i * 8, 4);
        memcpy(&value, input + i * 8 + 4, 4);
        cfg[i].Parameter = param;
        cfg[i].Value = value;
    }
    return n;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_j2534_discover_devices(ecuio_j2534_device_t** devices_out,
                                            uint32_t* device_count)
{
    if (devices_out == nullptr || device_count == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }

    *devices_out = nullptr;
    *device_count = 0;

    std::vector<ecuio_j2534_device_t> devices;
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

    std::vector<ecuio_j2534_device_t> unique;
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
        return ECUIO_OK;
    }

    try {
        ecuio_j2534_device_t* copy = new ecuio_j2534_device_t[unique.size()];
        std::copy(unique.begin(), unique.end(), copy);
        *devices_out = copy;
        *device_count = static_cast<uint32_t>(unique.size());
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

void ecuio_j2534_free_devices(ecuio_j2534_device_t* devices)
{
    delete[] devices;
}

ecuio_status_t ecuio_j2534_open(const ecuio_j2534_device_t* device,
                                ecuio_j2534_handle_t** out_handle)
{
    if (device == nullptr || out_handle == nullptr ||
        device->dll_path[0] == '\0') {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_handle = nullptr;

    /* ROUTING DECISION — made here, once. x86 DLLs go to the
     * surrogate (exe next to the app, or ECUIO_SURROGATE_EXE);
     * everything else, including unknown (sniff 0), loads in-proc, so
     * unreadable paths fail at LoadLibraryEx. A test can send every
     * DLL to the surrogate (ecuio_j2534_force_surrogate_for_testing). */
    const uint32_t arch = sniff_pe_architecture(device->dll_path);
    if (g_force_surrogate.load() || arch == ECUIO_DLL_ARCH_X86) {
        ecuio_j2534_sur_handle_t* sur = nullptr;
        ecuio_status_t st = ecuio_j2534_sur_open(device->dll_path, &sur);
        if (st != ECUIO_OK) {
            return st;   /* spawn/DLL-load/PassThruOpen failure — its
                            code and detail are set client-side */
        }
        ecuio_j2534_handle_t* h = new (std::nothrow) ecuio_j2534_handle_t();
        if (h == nullptr) {
            ecuio_j2534_sur_close(sur);
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        h->is_surrogate = true;
        h->dll_arch = arch;
        h->sur = sur;
        *out_handle = h;
        return ECUIO_OK;
    }

    wchar_t wpath[ECUIO_J2534_DLL_PATH_LEN];
    if (!ecuio_utf8_to_wide(device->dll_path, wpath, _countof(wpath))) {
        ecuio_detail_set("the DLL path isn't valid UTF-8, or is too long");
        return ECUIO_ERR_LOAD_FAILED;
    }
    HMODULE module = LoadLibraryExW(wpath, nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        ecuio_detail_set("couldn't load the DLL (Windows error %lu)",
                         GetLastError());
        return ECUIO_ERR_LOAD_FAILED;
    }

    ecuio_j2534_handle_t* h = new (std::nothrow) ecuio_j2534_handle_t();
    if (h == nullptr) {
        FreeLibrary(module);
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    h->module = module;
    h->dll_arch = arch;

    /* The nine v04.04 mandatory exports — the same set the surrogate
     * child requires, so both backends accept the same DLLs. */
    bool all_resolved = true;
    h->open          = resolve_export<fn_pt_open>(module, "PassThruOpen", all_resolved);
    h->close         = resolve_export<fn_pt_close>(module, "PassThruClose", all_resolved);
    h->connect       = resolve_export<fn_pt_connect>(module, "PassThruConnect", all_resolved);
    h->disconnect    = resolve_export<fn_pt_disconnect>(module, "PassThruDisconnect", all_resolved);
    h->read_msgs     = resolve_export<fn_pt_read_msgs>(module, "PassThruReadMsgs", all_resolved);
    h->write_msgs    = resolve_export<fn_pt_write_msgs>(module, "PassThruWriteMsgs", all_resolved);
    /* Spec spelling first, legacy spelling second. The surrogate
     * child has the same fallback; without it the backends would
     * disagree on which DLLs load. */
    h->ioctl         = resolve_export<fn_pt_ioctl>(module, "PassThruIoctl", all_resolved, "PassThruIoCtl");
    h->start_filter  = resolve_export<fn_pt_start_filter>(module, "PassThruStartMsgFilter", all_resolved);
    h->stop_filter   = resolve_export<fn_pt_stop_filter>(module, "PassThruStopMsgFilter", all_resolved);

    if (!all_resolved) {
        const char* missing =
            !h->open         ? "PassThruOpen"
          : !h->close        ? "PassThruClose"
          : !h->connect      ? "PassThruConnect"
          : !h->disconnect   ? "PassThruDisconnect"
          : !h->read_msgs    ? "PassThruReadMsgs"
          : !h->write_msgs   ? "PassThruWriteMsgs"
          : !h->ioctl        ? "PassThruIoctl"
          : !h->start_filter ? "PassThruStartMsgFilter"
          :                    "PassThruStopMsgFilter";
        ecuio_detail_set("the DLL doesn't export %s", missing);
        FreeLibrary(module);
        delete h;
        return ECUIO_ERR_LOAD_FAILED;
    }
    /* Optional, unlike the nine: a DLL without them loses only the
     * vendor's text and the session info's versions. The surrogate
     * child resolves them the same way. */
    h->get_last_error = reinterpret_cast<fn_pt_get_last_error>(
        GetProcAddress(module, "PassThruGetLastError"));
    h->read_version = reinterpret_cast<fn_pt_read_version>(
        GetProcAddress(module, "PassThruReadVersion"));

    unsigned long device_id = 0;
    const long open_rc = h->open(nullptr, &device_id);
    if (open_rc != kPtStatusNoError) {
        char text[ECUIO_PT_TEXT_LEN + 1];
        vendor_text(h, text);
        ecuio_detail_set_vendor("PassThruOpen", open_rc, text);
        FreeLibrary(module);
        delete h;
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    h->device_id = device_id;

    *out_handle = h;
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_close(ecuio_j2534_handle_t* handle)
{
    if (handle == nullptr) {
        return ECUIO_OK;
    }
    if (handle->is_surrogate) {
        /* Polite SHUTDOWN, bounded wait, job-object kill — all inside
         * the surrogate client. Deleting the box after is safe: the
         * transport already joined its worker before calling here. */
        ecuio_status_t st = ecuio_j2534_sur_close(handle->sur);
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
    return ECUIO_OK;
}

/* ---- Vendor-call helpers used by j2534_transport.cpp ---- */

ecuio_status_t ecuio_j2534_connect_channel(ecuio_j2534_handle_t* handle,
                                           unsigned long baud,
                                           unsigned long flags,
                                           unsigned long* out_channel)
{
    if (handle == nullptr || out_channel == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (handle->is_surrogate) {
        return ecuio_j2534_sur_connect_channel(handle->sur, baud,
                                               flags, out_channel);
    }
    /* baud == 0 is the transport-layer "unset" sentinel (the URI parser
     * rejects an explicit baud=0, so the ambiguity is closed by
     * contract); resolve it to the ISO 15765 default. flags pass
     * through verbatim — 0 is both the default and a legitimate value. */
    if (baud == 0) {
        baud = ECUIO_PT_BAUD_ISO15765_500K;
    }
    long rc = handle->connect(handle->device_id,
                              ECUIO_PT_PROTOCOL_ISO15765,
                              flags,
                              baud,
                              out_channel);
    if (rc != kPtStatusNoError) {
        char text[ECUIO_PT_TEXT_LEN + 1];
        vendor_text(handle, text);
        char call[64];
        snprintf(call, sizeof(call), "PassThruConnect (ISO 15765 at %lu baud)",
                 baud);
        ecuio_detail_set_vendor(call, rc, text);
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_disconnect_channel(ecuio_j2534_handle_t* handle,
                                              unsigned long channel)
{
    if (handle == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (handle->is_surrogate) {
        return ecuio_j2534_sur_disconnect_channel(handle->sur, channel);
    }
    /* Best-effort at teardown: vendor failure during shutdown must
     * not block the detach path. The status is deliberately
     * discarded. */
    (void)handle->disconnect(channel);
    return ECUIO_OK;
}

/* ---- Read frame: timeout vs empty vs hard-error distinction ----
 *
 *   NOERROR, no message            -> ECUIO_OK, no frame (idle)
 *   ERR_BUFFER_EMPTY (0x10)        -> ECUIO_OK, no frame (queue drained)
 *   ERR_TIMEOUT (0x09)             -> ECUIO_ERR_VEHICLE_TIMEOUT (quiet bus)
 *   any other nonzero             -> ECUIO_ERR_TRANSPORT_HARD_FAILURE
 *   num > 0 under NOERROR or      -> deliver the frame (J2534-1 pairs
 *     ERR_TIMEOUT                     ERR_TIMEOUT with data)
 *   an indication                 -> skipped, and the read goes on with
 *                                     what's left of its timeout
 *                                     (ecuio_pt_is_indication); the
 *                                     next message decides
 *   num > 0 under any other       -> count ignored, mapped as "no
 *     status                          message" above
 *                                     (ecuio_trusted_count: a vendor
 *                                     may leave the count untouched
 *                                     on error)
 *
 * The worker turns these codes into link health, which callers read
 * through ecuio_session_get_error_state (#11). */
ecuio_status_t ecuio_j2534_read_frame(ecuio_j2534_handle_t* handle,
                                      unsigned long channel,
                                      ecuio_frame_t* out_frame,
                                      uint32_t timeout_ms,
                                      bool* got_frame)
{
    if (handle == nullptr || out_frame == nullptr || got_frame == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *got_frame = false;

    if (handle->is_surrogate) {
        /* Surrogate path: the client maps pipe failure to
         * ECUIO_ERR_TRANSPORT_HARD_FAILURE and performs the same
         * timeout/empty/hard-error split on the batched OP_READ
         * status (#11). */
        return ecuio_j2534_sur_read_frame(handle->sur, channel,
                                          out_frame, timeout_ms,
                                          got_frame);
    }

    /* An indication doesn't end the read: read again with what's
     * left of the timeout, and at least once more, so a reply queued
     * behind it comes back from this call instead of the caller's
     * next one. The surrogate child does the same with its batches,
     * so both backends drain alike. */
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    PASSTHRU_MSG msg = {};
    unsigned long num = 0;
    long rc = kPtStatusNoError;
    uint32_t wait_ms = timeout_ms;
    for (uint32_t skipped = 0;;) {
        num = 1;
        rc = handle->read_msgs(channel, &msg, &num, wait_ms);
        num = ecuio_trusted_count(rc, num);    /* a count under a hard
                                                   error may be the vendor
                                                   leaving our 1 untouched */
        if (num == 0 || !ecuio_pt_is_indication(msg.RxStatus)) {
            break;
        }
        if (++skipped >= ECUIO_PT_MAX_SKIPPED_PER_READ) {
            num = 0;    /* indications only: judged like an empty read */
            break;
        }
        wait_ms = ms_left(deadline);
    }

    if (num == 0) {
        *got_frame = false;
        if (rc == kPtStatusNoError) {
            return ECUIO_OK;    /* idle */
        }
        if (rc == ECUIO_PT_STATUS_TIMEOUT) {
            return ECUIO_ERR_VEHICLE_TIMEOUT;
        }
        if (rc == ECUIO_PT_STATUS_BUFFER_EMPTY) {
            return ECUIO_OK;    /* queue empty — quiet, not failed */
        }
        return ECUIO_ERR_TRANSPORT_HARD_FAILURE;
    }

    /* num > 0: deliver even under ERR_TIMEOUT — the trust rule above
     * let the count through, and a conforming vendor returns data and
     * that status together. */
    *out_frame = ecuio_frame_t{};
    out_frame->arbitration_id = 0;
    out_frame->channel_id = 0;
    out_frame->flags = (msg.RxStatus & ECUIO_PT_CAN_29BIT_ID)
                        ? ECUIO_FRAME_FLAG_EXTENDED : 0u;
    out_frame->timestamp_us = msg.Timestamp;
    unsigned long n = msg.DataSize;
    if (n > sizeof(out_frame->data)) n = sizeof(out_frame->data);
    out_frame->data_length = static_cast<uint16_t>(n);
    memcpy(out_frame->data, msg.Data, n);
    *got_frame = true;
    return ECUIO_OK;
}

/* ---- Write / ioctl / filter vendor-call helpers ---- */

ecuio_status_t ecuio_j2534_write_frame(ecuio_j2534_handle_t* handle,
                                       unsigned long channel,
                                       const uint8_t* data,
                                       uint32_t len,
                                       uint32_t tx_flags,
                                       uint32_t timeout_ms,
                                       uint32_t* out_num_written)
{
    if (handle == nullptr || data == nullptr || len == 0 ||
        len > sizeof(((ecuio_frame_t*)0)->data)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out_num_written != nullptr) {
        *out_num_written = 0;
    }

    if (handle->is_surrogate) {
        return ecuio_j2534_sur_write_frame(handle->sur, channel, data,
                                             len, tx_flags, timeout_ms,
                                             out_num_written);
    }

    PASSTHRU_MSG msg;
    fill_vendor_msg(&msg, data, len, tx_flags);
    unsigned long num = 1;
    long rc = handle->write_msgs(channel, &msg, &num, timeout_ms);
    if (rc != kPtStatusNoError) {
        return ECUIO_ERR_NO_TRANSPORT;
    }
    /* Success-but-wrote-nothing is the caller's problem to catch via
     * out_num_written; we do not fabricate an error for a partial or
     * zero-count NOERROR return — the vendor said yes, the count
     * tells the truth. */
    if (out_num_written != nullptr) {
        *out_num_written = num;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_ioctl(ecuio_j2534_handle_t* handle,
                                  unsigned long channel,
                                  uint32_t ioctl_id,
                                  const uint8_t* input,
                                  uint32_t input_len,
                                  uint8_t* output,
                                  uint32_t output_cap,
                                  uint32_t* out_len)
{
    if (handle == nullptr || input_len > 64) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (input == nullptr && input_len != 0) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out_len != nullptr) {
        *out_len = 0;
    }

    /* Contract: only ioctls we can SHAPE correctly may cross this
     * seam. Pointer-bearing ids (FAST_INIT, FIVE_BAUD_INIT...) would
     * require vendor-specific struct marshaling — refusing beats
     * guessing. Transport-layer init sequences that need them get a
     * proper wire op if/when required. */
    const bool is_sconfig = (ioctl_id == ECUIO_PT_IOCTL_GET_CONFIG ||
                             ioctl_id == ECUIO_PT_IOCTL_SET_CONFIG);
    const bool is_null_form = (ioctl_id == ECUIO_PT_IOCTL_READ_VBATT ||
                               ioctl_id == ECUIO_PT_IOCTL_CLEAR_TX_BUFFER ||
                               ioctl_id == ECUIO_PT_IOCTL_CLEAR_RX_BUFFER ||
                               ioctl_id == ECUIO_PT_IOCTL_CLEAR_MSG_FILTERS);
    if (!is_sconfig && !is_null_form) {
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }

    if (handle->is_surrogate) {
        return ecuio_j2534_sur_ioctl(handle->sur, channel, ioctl_id, input,
                                     input_len, output, output_cap, out_len);
    }

    if (is_sconfig) {
        SCONFIG cfg[ECUIO_PT_SCONFIG_MAX_PARAMS];
        const uint32_t n = unflatten_sconfig(input, input_len, cfg,
                                             ECUIO_PT_SCONFIG_MAX_PARAMS);
        if (n == 0) {
            return ECUIO_ERR_NULL_ARGUMENT;    /* malformed flattened array */
        }
        SCONFIG_LIST list;
        list.NumOfParams = n;
        list.ConfigPtr = cfg;
        long rc = handle->ioctl(channel, static_cast<long>(ioctl_id),
                                &list, nullptr);
        if (rc != kPtStatusNoError) {
            return ECUIO_ERR_NO_TRANSPORT;
        }
        if (ioctl_id == ECUIO_PT_IOCTL_GET_CONFIG) {
            /* Re-flatten the vendor-updated pairs, as the child does. */
            uint8_t flat[ECUIO_PT_SCONFIG_MAX_PARAMS * 8];
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t param = static_cast<uint32_t>(cfg[i].Parameter);
                const uint32_t value = static_cast<uint32_t>(cfg[i].Value);
                memcpy(flat + i * 8, &param, 4);
                memcpy(flat + i * 8 + 4, &value, 4);
            }
            copy_out(output, output_cap, flat, n * 8);
            if (out_len != nullptr) {
                *out_len = n * 8;
            }
        }
        return ECUIO_OK;
    }

    if (ioctl_id == ECUIO_PT_IOCTL_READ_VBATT) {
        unsigned long vbatt = 0;
        /* v04.04: READ_VBATT is device-scoped — DeviceID, not the
         * channel. The surrogate child does the same. */
        long rc = handle->ioctl(handle->device_id,
                                static_cast<long>(ioctl_id),
                                nullptr, &vbatt);
        if (rc != kPtStatusNoError) {
            return ECUIO_ERR_NO_TRANSPORT;
        }
        const uint32_t mv = static_cast<uint32_t>(vbatt);
        copy_out(output, output_cap, &mv, 4);
        if (out_len != nullptr) {
            *out_len = 4;
        }
        return ECUIO_OK;
    }

    /* Remaining NULL/NULL forms: CLEAR_TX/RX_BUFFER, CLEAR_MSG_FILTERS */
    long rc = handle->ioctl(channel, static_cast<long>(ioctl_id),
                            nullptr, nullptr);
    return (rc == kPtStatusNoError) ? ECUIO_OK : ECUIO_ERR_NO_TRANSPORT;
}

ecuio_status_t ecuio_j2534_start_msg_filter(ecuio_j2534_handle_t* handle,
                                            unsigned long channel,
                                            uint32_t filter_type,
                                            const uint8_t* mask,
                                            uint32_t mask_len,
                                            const uint8_t* pattern,
                                            uint32_t pattern_len,
                                            const uint8_t* flow_control,
                                            uint32_t fc_len,
                                            uint32_t tx_flags,
                                            unsigned long* out_filter_id)
{
    if (handle == nullptr || mask == nullptr || pattern == nullptr ||
        out_filter_id == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    /* Spec: pattern 1..12 bytes, mask/pattern lengths equal, flow
     * control only with FLOW_CONTROL_FILTER and then also matching
     * lengths. Enforced at the seam — malformed filters must not
     * reach a real device, whatever the backend. */
    if (mask_len < 1 || mask_len > ECUIO_PT_FILTER_PATTERN_MAX ||
        mask_len != pattern_len ||
        fc_len > ECUIO_PT_FILTER_PATTERN_MAX) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (filter_type == ECUIO_PT_FLOW_CONTROL_FILTER) {
        if (fc_len != mask_len || flow_control == nullptr) {
            return ECUIO_ERR_NULL_ARGUMENT;
        }
    } else if (fc_len != 0) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_filter_id = 0;

    if (handle->is_surrogate) {
        return ecuio_j2534_sur_start_msg_filter(handle->sur, channel,
                                                filter_type, mask,
                                                mask_len, pattern,
                                                pattern_len, flow_control,
                                                fc_len, tx_flags,
                                                out_filter_id);
    }

    PASSTHRU_MSG mask_msg, pattern_msg, fc_msg;
    fill_vendor_msg(&mask_msg, mask, mask_len, tx_flags);
    fill_vendor_msg(&pattern_msg, pattern, pattern_len, tx_flags);
    PASSTHRU_MSG* fc_ptr = nullptr;
    if (fc_len > 0) {
        fill_vendor_msg(&fc_msg, flow_control, fc_len, tx_flags);
        fc_ptr = &fc_msg;
    }
    unsigned long filter_id = 0;
    long rc = handle->start_filter(channel, filter_type, &mask_msg,
                                   &pattern_msg, fc_ptr, &filter_id);
    if (rc != kPtStatusNoError) {
        char text[ECUIO_PT_TEXT_LEN + 1];
        vendor_text(handle, text);
        ecuio_detail_set_vendor("PassThruStartMsgFilter", rc, text);
        return ECUIO_ERR_VENDOR_REFUSED;
    }
    *out_filter_id = filter_id;
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_stop_msg_filter(ecuio_j2534_handle_t* handle,
                                           unsigned long channel,
                                           unsigned long filter_id)
{
    if (handle == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (handle->is_surrogate) {
        return ecuio_j2534_sur_stop_msg_filter(handle->sur, channel,
                                               filter_id);
    }
    /* Vendor status verbatim: stopping a stopped filter is the
     * vendor's ERR_FAILED, surfaced as ECUIO_ERR_NO_TRANSPORT — the
     * caller learns the truth rather than our guess about
     * idempotence. */
    long rc = handle->stop_filter(channel, filter_id);
    return (rc == kPtStatusNoError) ? ECUIO_OK : ECUIO_ERR_NO_TRANSPORT;
}

ecuio_status_t ecuio_j2534_read_version(ecuio_j2534_handle_t* handle,
                                        char* firmware,
                                        char* dll,
                                        char* api)
{
    if (handle == nullptr || firmware == nullptr || dll == nullptr ||
        api == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    char* const out[3] = {firmware, dll, api};
    for (char* o : out) {
        o[0] = '\0';
    }
    ecuio_status_t st = ECUIO_OK;
    if (handle->is_surrogate) {
        st = ecuio_j2534_sur_read_version(handle->sur, firmware, dll, api);
    } else if (handle->read_version == nullptr) {
        st = ECUIO_ERR_NOT_IMPLEMENTED;
    } else {
        /* Room past the spec's 80 for a vendor that overruns it, as for
         * PassThruGetLastError. */
        char text[3][256] = {};
        if (handle->read_version(handle->device_id, text[0], text[1],
                                 text[2]) != kPtStatusNoError) {
            st = ECUIO_ERR_NO_TRANSPORT;
        } else {
            for (int i = 0; i < 3; ++i) {
                text[i][ECUIO_SESSION_INFO_TEXT_LEN - 1] = '\0';
                memcpy(out[i], text[i], ECUIO_SESSION_INFO_TEXT_LEN);
            }
        }
    }
    if (st != ECUIO_OK) {
        for (char* o : out) {
            o[0] = '\0';
        }
        return st;
    }
    for (char* o : out) {
        ecuio_text_one_line(o);
    }
    return ECUIO_OK;
}

const char* ecuio_j2534_backend_name(const ecuio_j2534_handle_t* handle)
{
    if (handle == nullptr) {
        return nullptr;
    }
    return handle->is_surrogate ? "surrogate" : "inproc";
}

uint32_t ecuio_j2534_backend(const ecuio_j2534_handle_t* handle)
{
    if (handle == nullptr) {
        return 0;
    }
    return handle->is_surrogate ? ECUIO_BACKEND_J2534_SURROGATE
                                : ECUIO_BACKEND_J2534_INPROC;
}

uint32_t ecuio_j2534_dll_arch(const ecuio_j2534_handle_t* handle)
{
    return (handle != nullptr) ? handle->dll_arch : ECUIO_DLL_ARCH_UNKNOWN;
}

/* TEST-ONLY: forward an injected frame into the loaded module's
 * InjectFrame hook (fake DLL). Routes by backend like everything
 * else: surrogate mode crosses the control pipe into the child's
 * module instance; in-proc mode resolves InjectFrame lazily — it is
 * a test hook, not a J2534 mandatory export, so a DLL without it
 * yields ECUIO_ERR_NOT_IMPLEMENTED rather than an open-time failure. */
ecuio_status_t ecuio_j2534_inject_test_frame(ecuio_j2534_handle_t* handle,
                                             const uint8_t* data,
                                             uint32_t len,
                                             uint32_t timestamp,
                                             uint32_t rx_flags)
{
    if (handle == nullptr || data == nullptr || len == 0 || len > 64) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (handle->is_surrogate) {
        return ecuio_j2534_sur_inject_test_frame(handle->sur, data, len,
                                                 timestamp, rx_flags);
    }
    if (handle->module == nullptr) {
        return ECUIO_ERR_NO_TRANSPORT;
    }
    typedef void (*fn_inject)(const unsigned char*, unsigned long,
                              unsigned long, unsigned long, unsigned long);
    fn_inject inject = reinterpret_cast<fn_inject>(
        GetProcAddress(handle->module, "InjectFrame"));
    if (inject == nullptr) {
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }
    inject(data, len, timestamp, rx_flags, 0);
    return ECUIO_OK;
}

void ecuio_j2534_force_surrogate_for_testing(bool on)
{
    g_force_surrogate.store(on);
}

} // extern "C"

#else

extern "C" {

ecuio_status_t ecuio_j2534_discover_devices(ecuio_j2534_device_t** devices_out,
                                            uint32_t* device_count)
{
    if (devices_out == nullptr || device_count == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *devices_out = nullptr;
    *device_count = 0;
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

void ecuio_j2534_free_devices(ecuio_j2534_device_t* /*devices*/)
{
}

ecuio_status_t ecuio_j2534_open(const ecuio_j2534_device_t* /*device*/,
                                ecuio_j2534_handle_t** out_handle)
{
    if (out_handle != nullptr) {
        *out_handle = nullptr;
    }
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_j2534_close(ecuio_j2534_handle_t* /*handle*/)
{
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_j2534_connect_channel(ecuio_j2534_handle_t* /*h*/,
                                           unsigned long /*baud*/,
                                           unsigned long /*flags*/,
                                           unsigned long* /*ch*/)
{
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_j2534_disconnect_channel(ecuio_j2534_handle_t* /*h*/,
                                              unsigned long /*ch*/)
{
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_j2534_read_frame(ecuio_j2534_handle_t* /*h*/,
                                      unsigned long /*ch*/,
                                      ecuio_frame_t* /*f*/,
                                      uint32_t /*t*/,
                                      bool* got_frame)
{
    if (got_frame != nullptr) {
        *got_frame = false;
    }
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_j2534_write_frame(ecuio_j2534_handle_t* /*h*/,
                                       unsigned long /*ch*/,
                                       const uint8_t* /*d*/,
                                       uint32_t /*len*/,
                                       uint32_t /*flags*/,
                                       uint32_t /*timeout*/,
                                       uint32_t* out_num_written)
{
    if (out_num_written != nullptr) {
        *out_num_written = 0;
    }
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_j2534_ioctl(ecuio_j2534_handle_t* /*h*/,
                                 unsigned long /*ch*/,
                                 uint32_t /*id*/,
                                 const uint8_t* /*in*/,
                                 uint32_t /*in_len*/,
                                 uint8_t* /*out*/,
                                 uint32_t /*cap*/,
                                 uint32_t* out_len)
{
    if (out_len != nullptr) {
        *out_len = 0;
    }
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_j2534_start_msg_filter(ecuio_j2534_handle_t* /*h*/,
                                            unsigned long /*ch*/,
                                            uint32_t /*type*/,
                                            const uint8_t* /*mask*/,
                                            uint32_t /*mask_len*/,
                                            const uint8_t* /*pattern*/,
                                            uint32_t /*pattern_len*/,
                                            const uint8_t* /*fc*/,
                                            uint32_t /*fc_len*/,
                                            uint32_t /*tx_flags*/,
                                            unsigned long* out_filter_id)
{
    if (out_filter_id != nullptr) {
        *out_filter_id = 0;
    }
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_j2534_stop_msg_filter(ecuio_j2534_handle_t* /*h*/,
                                           unsigned long /*ch*/,
                                           unsigned long /*fid*/)
{
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

ecuio_status_t ecuio_j2534_read_version(ecuio_j2534_handle_t* /*h*/,
                                        char* firmware,
                                        char* dll,
                                        char* api)
{
    if (firmware != nullptr) firmware[0] = '\0';
    if (dll != nullptr) dll[0] = '\0';
    if (api != nullptr) api[0] = '\0';
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

const char* ecuio_j2534_backend_name(const ecuio_j2534_handle_t* /*handle*/)
{
    return nullptr;
}

uint32_t ecuio_j2534_backend(const ecuio_j2534_handle_t* /*handle*/)
{
    return 0;
}

uint32_t ecuio_j2534_dll_arch(const ecuio_j2534_handle_t* /*handle*/)
{
    return ECUIO_DLL_ARCH_UNKNOWN;
}

ecuio_status_t ecuio_j2534_inject_test_frame(ecuio_j2534_handle_t* /*h*/,
                                              const uint8_t* /*d*/,
                                              uint32_t /*len*/,
                                              uint32_t /*ts*/,
                                              uint32_t /*flags*/)
{
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

void ecuio_j2534_force_surrogate_for_testing(bool /*on*/)
{
}

} // extern "C"

#endif /* _WIN32 */
