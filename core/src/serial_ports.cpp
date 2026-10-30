// File/layer: core/src/serial_ports.cpp | C++17, Qt-free
// Testing: UNIT: SerialPortsTest (test_elm327): the arguments, and the
//          contract against the machine's own ports (none on most CI
//          runners); the rules over made-up ports are serial_ports.h's.
//          The Windows registry read and the /dev scan on a machine with
//          an adapter are manual smoke (Settings) and BENCH
//
// ecuio_serial_ports (serial.h): gathers the candidates the OS lists and
// hands them to serial_ports_finish. Nothing here opens a port.

#include "ecuio/serial.h"

#include "elm327_port.h"
#include "serial_ports.h"

#include <cstring>
#include <new>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include "win_utf8.h"
#else
#include <dirent.h>
#endif

namespace {

/* Adds name (with its device) when elm327:// takes it */
void consider(const char* name, const char* device,
              std::vector<ecuio_serial_port_t>& found)
{
    if (!elm327_port_name_ok(name, strlen(name))) {
        return;
    }
    ecuio_serial_port_t port;
    if (serial_port_make(name, device, &port)) {
        found.push_back(port);
    }
}

#ifdef _WIN32

/* SERIALCOMM's values: each name the driver's device ("\Device\VCP0"),
 * each value its port ("COM5"). Read as UTF-16, as discovery's
 * FUNCTIONLIBRARY is. No key means no ports. */
void gather(std::vector<ecuio_serial_port_t>& found)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t device[ECUIO_SERIAL_DEVICE_LEN] = {};
        DWORD device_len = ECUIO_SERIAL_DEVICE_LEN;
        wchar_t port[ECUIO_SERIAL_NAME_LEN] = {};
        DWORD port_size = static_cast<DWORD>(sizeof(port) - sizeof(wchar_t));
        DWORD type = 0;
        const LONG rc =
            RegEnumValueW(key, i, device, &device_len, nullptr, &type,
                          reinterpret_cast<LPBYTE>(port), &port_size);
        if (rc == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (rc != ERROR_SUCCESS || type != REG_SZ) {
            continue; /* a name or port too long, or not text */
        }
        /* The stored string need not be terminated. size is in bytes. */
        port[port_size / sizeof(wchar_t)] = L'\0';
        char name8[ECUIO_SERIAL_NAME_LEN] = {};
        char device8[ECUIO_SERIAL_DEVICE_LEN] = {};
        if (!ecuio_wide_to_utf8(port, name8, sizeof(name8))) {
            continue;
        }
        (void)ecuio_wide_to_utf8(device, device8, sizeof(device8));
        consider(name8, device8, found);
    }
    RegCloseKey(key);
}

#else

/* The /dev entries serial_dev_listed names; no /dev means no ports. */
void gather(std::vector<ecuio_serial_port_t>& found)
{
#ifdef __APPLE__
    constexpr bool apple = true;
#else
    constexpr bool apple = false;
#endif
    DIR* dir = opendir("/dev");
    if (dir == nullptr) {
        return;
    }
    while (const dirent* entry = readdir(dir)) {
        if (!serial_dev_listed(entry->d_name, apple)) {
            continue;
        }
        char path[ECUIO_SERIAL_NAME_LEN] = "/dev/";
        const size_t n = strlen(entry->d_name);
        if (n + 5 >= sizeof(path)) {
            continue;
        }
        memcpy(path + 5, entry->d_name, n + 1);
        consider(path, "", found);
    }
    closedir(dir);
}

#endif

} // namespace

extern "C" ecuio_status_t ecuio_serial_ports(ecuio_serial_port_t* ports,
                                             uint32_t capacity, uint32_t* count)
{
    if (count == nullptr || (ports == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = 0;
    try {
        std::vector<ecuio_serial_port_t> found;
        gather(found);
        serial_ports_finish(found, ports, capacity, count);
    } catch (const std::bad_alloc&) {
        *count = 0;
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}
