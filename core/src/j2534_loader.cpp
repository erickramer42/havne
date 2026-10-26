// File/layer: core/src/j2534_loader.cpp | C++17, Qt-free, CI-validated headless
// Testing: HARNESS — live-registry discovery tests, test_j2534_load
//          (load/export/open/close), test_j2534_stream (end to end),
//          test_j2534_routing (the surrogate by PE architecture, the
//          refusals before any spawn), J2534ConnectTest.MissingDllFailsToLoad
//          (the sniff's three outcomes) and
//          J2534ConnectTest.ARelativeDllPathIsRefusedBeforeAnyLoad
//          (discovery's skip by the CI registry's relative entry)
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
#include <vector>

/* Backing type for the header's opaque ecuio_j2534_handle_t. At global
 * scope, never in the anonymous namespace: the header's typedef targets
 * the global tag name (C2027 otherwise).
 *
 * ecuio_j2534_open sniffs the PE architecture once and loads the DLL in
 * a surrogate of that architecture (ADR-0025); nothing loads in this
 * process. Every helper below checks its arguments at this seam and
 * forwards to the surrogate client, so the transport layer
 * (j2534_transport.cpp) never knows which surrogate serves a session. */
struct ecuio_j2534_handle {
    uint32_t dll_arch = ECUIO_DLL_ARCH_UNKNOWN;   /* the PE sniff at open */
    ecuio_j2534_sur_handle_t* sur = nullptr;      /* owned */
};

namespace {

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
    /* A relative path is skipped: ecuio_j2534_open refuses one. */
    if (!ecuio_is_absolute_path(path)) {
        return false;
    }
    /* A path whose UTF-8 form won't fit the device struct is skipped,
     * not truncated: a truncated path loads the wrong file or none. */
    return ecuio_wide_to_utf8(path, out, out_size) && out[0] != '\0';
}

/* What a DLL's headers say (ADR-0025): unreadable (error set, 0 for a
 * path that isn't UTF-8 or is too long), not a PE, or a PE whose
 * machine is x86, x64 or another (ECUIO_DLL_ARCH_UNKNOWN). */
struct PeSniff {
    enum Kind { kUnreadable, kNotPe, kPe };
    Kind kind = kUnreadable;
    uint32_t arch = ECUIO_DLL_ARCH_UNKNOWN;
    DWORD error = 0;
};

/* ecuio_j2534_open routes on it: the unreadable and the non-PE are
 * refused there, with no spawn; x86 goes to the x86 surrogate and every
 * other PE to the x64 one. The discovery annotation is only a UI label
 * (its arch); the sniff in open() is authoritative and also covers
 * direct-path selectors. */
PeSniff sniff_pe(const char* dll_path)
{
    PeSniff out;
    wchar_t wpath[ECUIO_J2534_DLL_PATH_LEN];
    if (!ecuio_utf8_to_wide(dll_path, wpath, _countof(wpath))) {
        return out;
    }
    HANDLE hFile = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        out.error = GetLastError();
        return out;
    }

    out.kind = PeSniff::kNotPe;
    DWORD bytes_read;
    IMAGE_DOS_HEADER dos_hdr = {};
    if (!ReadFile(hFile, &dos_hdr, sizeof(dos_hdr), &bytes_read, NULL) ||
        bytes_read != sizeof(dos_hdr) || dos_hdr.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(hFile);
        return out;
    }

    SetFilePointer(hFile, dos_hdr.e_lfanew, NULL, FILE_BEGIN);
    IMAGE_NT_HEADERS nt_hdr = {};
    if (!ReadFile(hFile, &nt_hdr, sizeof(nt_hdr), &bytes_read, NULL) ||
        bytes_read < sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) ||
        nt_hdr.Signature != IMAGE_NT_SIGNATURE) {
        CloseHandle(hFile);
        return out;
    }
    CloseHandle(hFile);

    out.kind = PeSniff::kPe;
    const uint16_t machine = nt_hdr.FileHeader.Machine;
    if (machine == IMAGE_FILE_MACHINE_I386) {
        out.arch = ECUIO_DLL_ARCH_X86;
    } else if (machine == IMAGE_FILE_MACHINE_AMD64) {
        out.arch = ECUIO_DLL_ARCH_X64;
    }   /* ARM64, Itanium, …: UNKNOWN, hosted by the x64 surrogate */
    return out;
}

/* Discovery's label: 1 = x86, 2 = x64, 0 = anything else (j2534.h's
 * reserved[0] contract). */
uint32_t sniff_pe_architecture(const char* dll_path)
{
    return sniff_pe(dll_path).arch;
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

    /* Every path reaches the child's LoadLibraryEx with
     * LOAD_WITH_ALTERED_SEARCH_PATH, and a relative one follows the
     * search order, the current directory included. Checked here, before
     * the architecture sniff opens the file, so a URI's selector, a
     * registry entry and a direct caller meet the same rule. */
    if (!ecuio_is_absolute_path(device->dll_path)) {
        ecuio_detail_set("the DLL path must be absolute, like "
                         "C:\\path\\to\\vendor.dll");
        return ECUIO_ERR_LOAD_FAILED;
    }

    /* ROUTING DECISION — made here, once (ADR-0025). A file that can't
     * be a DLL is refused with no spawn. Every PE loads in a surrogate:
     * x86 in the x86 one (next to the app, or ECUIO_SURROGATE_EXE),
     * x64 and any other machine in the x64 one
     * (ECUIO_SURROGATE_X64_EXE), which loads or refuses it as the app's
     * own process would. */
    const PeSniff pe = sniff_pe(device->dll_path);
    if (pe.kind == PeSniff::kUnreadable) {
        if (pe.error == 0) {
            ecuio_detail_set("the DLL path isn't valid UTF-8, or is too long");
        } else {
            ecuio_detail_set("couldn't open the DLL (Windows error %lu)",
                             pe.error);
        }
        return ECUIO_ERR_LOAD_FAILED;
    }
    if (pe.kind == PeSniff::kNotPe) {
        ecuio_detail_set("the file isn't a Windows DLL");
        return ECUIO_ERR_LOAD_FAILED;
    }

    ecuio_j2534_sur_handle_t* sur = nullptr;
    const ecuio_status_t st = ecuio_j2534_sur_open(device->dll_path, pe.arch,
                                                   &sur);
    if (st != ECUIO_OK) {
        return st;   /* spawn/DLL-load/PassThruOpen failure — its
                        code and detail are set client-side */
    }
    ecuio_j2534_handle_t* h = new (std::nothrow) ecuio_j2534_handle_t();
    if (h == nullptr) {
        ecuio_j2534_sur_close(sur);
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    h->dll_arch = pe.arch;
    h->sur = sur;
    *out_handle = h;
    return ECUIO_OK;
}

ecuio_status_t ecuio_j2534_close(ecuio_j2534_handle_t* handle)
{
    if (handle == nullptr) {
        return ECUIO_OK;
    }
    /* Polite SHUTDOWN, bounded wait, job-object kill — all inside the
     * surrogate client. Deleting the box after is safe: the transport
     * already joined its worker before calling here. */
    const ecuio_status_t st = ecuio_j2534_sur_close(handle->sur);
    delete handle;
    return st;
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
    /* baud == 0 is the transport's "unset" sentinel; the client resolves
     * it to the ISO 15765 default. flags pass through verbatim. */
    return ecuio_j2534_sur_connect_channel(handle->sur, baud, flags,
                                           out_channel);
}

ecuio_status_t ecuio_j2534_disconnect_channel(ecuio_j2534_handle_t* handle,
                                              unsigned long channel)
{
    if (handle == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    /* Best-effort at teardown: the client discards the vendor's status. */
    return ecuio_j2534_sur_disconnect_channel(handle->sur, channel);
}

/* ---- Read a message: timeout vs empty vs hard-error distinction ----
 *
 *   NOERROR, no message            -> ECUIO_OK, no frame (idle)
 *   ERR_BUFFER_EMPTY (0x10)        -> ECUIO_OK, no frame (queue drained)
 *   ERR_TIMEOUT (0x09)             -> ECUIO_ERR_VEHICLE_TIMEOUT (quiet bus)
 *   any other nonzero             -> ECUIO_ERR_TRANSPORT_HARD_FAILURE
 *   num > 0 under NOERROR or      -> deliver the message (J2534-1 pairs
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
 * The child reads in batches (OP_READ) and skips indications; the
 * client applies this mapping to the batch's status and maps a broken
 * pipe to ECUIO_ERR_TRANSPORT_HARD_FAILURE. The worker turns these
 * codes into link health, which callers read through
 * ecuio_session_get_error_state. */
ecuio_status_t ecuio_j2534_read_msg(ecuio_j2534_handle_t* handle,
                                    unsigned long channel,
                                    ecuio_pt_rx_t* out_msg,
                                    uint32_t timeout_ms,
                                    bool* got_msg)
{
    if (handle == nullptr || out_msg == nullptr || got_msg == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *got_msg = false;
    return ecuio_j2534_sur_read_msg(handle->sur, channel, out_msg,
                                    timeout_ms, got_msg);
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
    if (handle == nullptr || data == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (len == 0 || len > ECUIO_PT_WRITE_MAX) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (out_num_written != nullptr) {
        *out_num_written = 0;
    }

    /* A NOERROR that wrote nothing comes back as ECUIO_OK with the
     * count: the vendor said yes, the count tells the truth, and the
     * caller judges it (send_request). */
    return ecuio_j2534_sur_write_frame(handle->sur, channel, data, len,
                                       tx_flags, timeout_ms, out_num_written);
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
    if (handle == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (input_len > ECUIO_PT_SCONFIG_MAX_PARAMS * 8) {
        return ECUIO_ERR_INVALID_ARGUMENT;
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
    /* A flattened SCONFIG array is whole pairs, 1 to
     * ECUIO_PT_SCONFIG_MAX_PARAMS of them. Judged here, before the
     * pipe; the child checks again at its trust boundary. */
    if (is_sconfig && (input_len == 0 || (input_len % 8) != 0 ||
                       input_len / 8 > ECUIO_PT_SCONFIG_MAX_PARAMS)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }

    /* The child unflattens the pairs, sends READ_VBATT to the device
     * ID (v04.04), and the client copies the first output_cap bytes. */
    return ecuio_j2534_sur_ioctl(handle->sur, channel, ioctl_id, input,
                                 input_len, output, output_cap, out_len);
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
     * reach a real device. */
    if (mask_len < 1 || mask_len > ECUIO_PT_FILTER_PATTERN_MAX ||
        mask_len != pattern_len ||
        fc_len > ECUIO_PT_FILTER_PATTERN_MAX) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (filter_type == ECUIO_PT_FLOW_CONTROL_FILTER) {
        if (flow_control == nullptr) {
            return ECUIO_ERR_NULL_ARGUMENT;
        }
        if (fc_len != mask_len) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    } else if (fc_len != 0) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out_filter_id = 0;
    return ecuio_j2534_sur_start_msg_filter(handle->sur, channel, filter_type,
                                            mask, mask_len, pattern,
                                            pattern_len, flow_control, fc_len,
                                            tx_flags, out_filter_id);
}

ecuio_status_t ecuio_j2534_stop_msg_filter(ecuio_j2534_handle_t* handle,
                                           unsigned long channel,
                                           unsigned long filter_id)
{
    if (handle == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    /* Not idempotent: stopping a stopped filter is a strict vendor's
     * ERR_FAILED, passed on as ECUIO_ERR_VENDOR_REFUSED rather than our
     * guess about idempotence. */
    return ecuio_j2534_sur_stop_msg_filter(handle->sur, channel, filter_id);
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
    const ecuio_status_t st =
        ecuio_j2534_sur_read_version(handle->sur, firmware, dll, api);
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

uint32_t ecuio_j2534_backend(const ecuio_j2534_handle_t* handle)
{
    return (handle != nullptr) ? ECUIO_BACKEND_J2534_SURROGATE : 0;
}

uint32_t ecuio_j2534_dll_arch(const ecuio_j2534_handle_t* handle)
{
    return (handle != nullptr) ? handle->dll_arch : ECUIO_DLL_ARCH_UNKNOWN;
}

/* TEST-ONLY: forward an injected frame over the pipe into the child's
 * fake DLL (OP_INJECT). InjectFrame is a test hook, not a J2534 export,
 * so the child resolves it lazily and a DLL without it answers
 * ECUIO_ERR_NOT_IMPLEMENTED rather than failing the open. */
ecuio_status_t ecuio_j2534_inject_test_frame(ecuio_j2534_handle_t* handle,
                                             const uint8_t* data,
                                             uint32_t len,
                                             uint32_t timestamp,
                                             uint32_t rx_flags)
{
    if (handle == nullptr || data == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (len == 0 || len > ECUIO_PT_WRITE_MAX) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ecuio_j2534_sur_inject_test_frame(handle->sur, data, len,
                                             timestamp, rx_flags);
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

ecuio_status_t ecuio_j2534_read_msg(ecuio_j2534_handle_t* /*h*/,
                                    unsigned long /*ch*/,
                                    ecuio_pt_rx_t* /*m*/,
                                    uint32_t /*t*/,
                                    bool* got_msg)
{
    if (got_msg != nullptr) {
        *got_msg = false;
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

} // extern "C"

#endif /* _WIN32 */
