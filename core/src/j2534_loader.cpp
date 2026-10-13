// File/layer: core/src/j2534_loader.cpp | C++17, Win32-guarded
// Testing: HARNESS — registry live-machine tests + test_j2534_load
//          against the fake vendor DLL (real LoadLibraryEx cycle)
// CI: compiled out entirely on non-Windows (CMake target_sources guard)

#include "qttune/j2534.h"
#include "qttune/core.h"

#ifdef _WIN32

#include <windows.h>

#include <vector>
#include <algorithm>

/* ================================================================
 * GLOBAL SCOPE — completes types the header forward-declares.
 * Order matters: PASSTHRU_MSG_FWD -> typedefs -> handle struct.
 * Nothing here may sit inside an anonymous namespace; the header's
 * typedef targets the global tag name and anything internal-linkage
 * becomes a distinct, never-completed type (C2027 trap).
 * ================================================================ */

/* Opaque forward — Commit C replaces with the real PASSTHRU_MSG
 * layout shared with the fake DLL. */
struct PASSTHRU_MSG_FWD;

/* SAE J2534-1 signature typedefs. `long` status returns, 32-bit
 * `unsigned long` IDs. __stdcall matches 32-bit vendor DLLs; on
 * x64/ARM64 MSVC folds it to the default convention, identical to
 * the fake DLL's exports. */
typedef long (__stdcall *fn_pt_open)(void* pName, unsigned long* pDeviceID);
typedef long (__stdcall *fn_pt_close)(unsigned long DeviceID);
typedef long (__stdcall *fn_pt_connect)(unsigned long DeviceID,
                                        unsigned long ProtocolID,
                                        unsigned long Flags,
                                        unsigned long BaudRate,
                                        unsigned long* pChannelID);
typedef long (__stdcall *fn_pt_disconnect)(unsigned long ChannelID);
typedef long (__stdcall *fn_pt_read_msgs)(unsigned long ChannelID,
                                          PASSTHRU_MSG_FWD* pMsg,
                                          unsigned long* pNumMsgs,
                                          unsigned long Timeout);
typedef long (__stdcall *fn_pt_write_msgs)(unsigned long ChannelID,
                                           PASSTHRU_MSG_FWD* pMsg,
                                           unsigned long* pNumMsgs,
                                           unsigned long Timeout);
typedef long (__stdcall *fn_pt_ioctl)(unsigned long ChannelID,
                                      long IoControlID,
                                      void* pInput,
                                      void* pOutput);

/* Backing type for the header's opaque qttune_j2534_handle_t. */
struct qttune_j2534_handle {
    HMODULE module;
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

/* SAE J2534-1 (v4.04) status codes (subset). */
constexpr long kPtStatusNoError = 0;
constexpr long kPtStatusFailed  = 0xC0000000L;

/* resolve_export keeps open()'s failure path (FreeLibrary + delete)
 * provably single-site. */
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

/* SAE J2534-1 (v4.04) function-library registration root.
 * Vendor subkeys sit DIRECTLY under this key (no "Vendors" level —
 * corrects an earlier draft; verified against the spec's mandated path).
 * Dual-view enumeration uses WOW64 access flags, not a hardcoded
 * WOW6432Node path, so it behaves correctly from any process bitness. */
constexpr const wchar_t* kPassThruRoot = L"SOFTWARE\\Pass-Thru Support.04.04";

/* Read the vendor's FUNCTIONLIBRARY path (REG_SZ, ANSI query converts). */
bool read_function_library(HKEY hvendor, char* out, size_t out_size)
{
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(out_size);
    LONG rc = RegQueryValueExA(hvendor, "FUNCTIONLIBRARY", nullptr,
                               &type, reinterpret_cast<LPBYTE>(out), &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    /* Guarantee termination; registry strings may omit it in edge cases */
    out[out_size - 1] = '\0';
    if (size > 0 && out[size - 1] != '\0' && size < out_size) {
        out[size] = '\0';
    }
    return out[0] != '\0';   /* empty value = unusable entry */
}

/* Enumerate one registry view (KEY_WOW64_64KEY or KEY_WOW64_32KEY). */
void enumerate_view(REGSAM wow64_flag,
                    std::vector<qttune_j2534_device_t>& out_devices)
{
    HKEY hroot = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPassThruRoot, 0,
                            KEY_READ | wow64_flag, &hroot);
    if (rc != ERROR_SUCCESS || hroot == nullptr) {
        return;   /* key absent in this view — no vendors, not an error */
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

        /* Vendor name = registry subkey name, narrowed to UTF-8 */
        WideCharToMultiByte(CP_UTF8, 0, subkey_name, -1,
                            dev.vendor_name, QTTUNE_J2534_VENDOR_NAME_LEN,
                            nullptr, nullptr);

        if (read_function_library(hvendor, dev.dll_path,
                                  sizeof(dev.dll_path))) {
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

    enumerate_view(KEY_WOW64_64KEY, devices);
    enumerate_view(KEY_WOW64_32KEY, devices);

    /* De-duplicate across views: 64-bit DLLs can register under both
     * views in edge cases, and vendor_id must be stable. Match on
     * dll_path (case-insensitive on Windows) AND vendor name. */
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
        d.vendor_id = static_cast<uint32_t>(&d - &unique[0]); /* re-index */
    }

    if (unique.empty()) {
        return QT_OK;   /* no vendors — not an error */
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

    /* LOAD_WITH_ALTERED_SEARCH_PATH: vendor DLLs frequently ship
     * support DLLs beside them; default search order misses those
     * when launched from a different cwd. Requires an absolute
     * path (which the registry contract guarantees). */
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
    /* ORDER MATTERS: vendor call while the module is still mapped,
     * then unmap, then free the struct holding now-dangling pointers. */
    if (handle->close != nullptr) {
        handle->close(handle->device_id);
    }
    if (handle->module != nullptr) {
        FreeLibrary(handle->module);
    }
    delete handle;
    return QT_OK;
}

} // extern "C"

#else

/* Non-Windows stubs — keeps the header contract satisfied so tests
 * compile everywhere; discovery and loading are inherently Win32. */
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

} // extern "C"

#endif /* _WIN32 */
