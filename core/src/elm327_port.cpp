// File/layer: core/src/elm327_port.cpp | C++17, Qt-free; Win32 and POSIX
//             halves, one built per platform
// Testing: HARNESS: Elm327PortTest (test_elm327, POSIX): the termios
//          port over a pseudo-terminal pair; Elm327TcpTest (every
//          platform): an address reaching the TCP opener; BENCH: the
//          Win32 port (ADR-0026 §8: Windows has no built-in virtual COM
//          pair, a known gap)

#include "elm327_port.h"
#include "elm327_tcp.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace {

/* A write that can't finish in this long has failed: the adapter or the
 * link stopped taking bytes. */
constexpr uint32_t ELM327_PORT_WRITE_TIMEOUT_MS = 2000;

void set_why(char* why, uint32_t cap, const char* text)
{
    if (cap > 0) {
        snprintf(why, cap, "%s", text);
    }
}

} // namespace

#ifdef _WIN32

#include "win_utf8.h" /* <windows.h> */

namespace {

struct win_port {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool timeouts_set = false;
    uint32_t read_timeout_ms = 0; /* what COMMTIMEOUTS holds */
};

/* Reads that return what has arrived at once, or wait up to timeout_ms
 * for the first byte and return with it (the documented
 * MAXDWORD/MAXDWORD/constant form); timeout 0 never waits. */
bool set_read_timeout(win_port* p, uint32_t timeout_ms)
{
    if (p->timeouts_set && p->read_timeout_ms == timeout_ms) {
        return true;
    }
    COMMTIMEOUTS ct{};
    ct.ReadIntervalTimeout = MAXDWORD;
    ct.ReadTotalTimeoutMultiplier = (timeout_ms == 0) ? 0 : MAXDWORD;
    ct.ReadTotalTimeoutConstant = timeout_ms;
    ct.WriteTotalTimeoutConstant = ELM327_PORT_WRITE_TIMEOUT_MS;
    if (!SetCommTimeouts(p->handle, &ct)) {
        return false;
    }
    p->timeouts_set = true;
    p->read_timeout_ms = timeout_ms;
    return true;
}

bool win_write(void* impl, const uint8_t* data, uint32_t len)
{
    auto* p = static_cast<win_port*>(impl);
    while (len > 0) {
        DWORD done = 0;
        if (!WriteFile(p->handle, data, len, &done, nullptr) || done == 0) {
            return false;
        }
        data += done;
        len -= done;
    }
    return true;
}

int32_t win_read(void* impl, uint8_t* buf, uint32_t cap, uint32_t timeout_ms)
{
    auto* p = static_cast<win_port*>(impl);
    if (!set_read_timeout(p, timeout_ms)) {
        return -1;
    }
    DWORD got = 0;
    if (!ReadFile(p->handle, buf, cap, &got, nullptr)) {
        return -1;
    }
    return static_cast<int32_t>(got);
}

void win_close(void* impl)
{
    auto* p = static_cast<win_port*>(impl);
    CloseHandle(p->handle);
    delete p;
}

const ecuio_elm327_port_ops_t kWinPortOps = {win_write, win_read, win_close};

} // namespace

bool elm327_port_name_ok(const char* name, size_t n)
{
    if (n < 4 || n > 6 || (name[0] != 'C' && name[0] != 'c') ||
        (name[1] != 'O' && name[1] != 'o') ||
        (name[2] != 'M' && name[2] != 'm') || name[3] == '0') {
        return false;
    }
    unsigned v = 0;
    for (size_t i = 3; i < n; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        v = v * 10 + static_cast<unsigned>(name[i] - '0');
    }
    return v >= 1 && v <= 255;
}

const char* elm327_port_name_example()
{
    return "COM1 to COM255, like COM5";
}

namespace {

ecuio_status_t serial_open(const char* name, uint32_t serial_baud,
                           ecuio_elm327_port_t* out, char* why,
                           uint32_t why_cap)
{
    /* \\.\COMn opens every port, past COM9 too */
    wchar_t path[16] = L"\\\\.\\";
    size_t k = 4;
    for (size_t i = 0; name[i] != '\0' && k + 1 < 16; ++i) {
        path[k++] = static_cast<wchar_t>(name[i]);
    }
    path[k] = L'\0';
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            set_why(why, why_cap, "no such port");
        } else if (err == ERROR_ACCESS_DENIED ||
                   err == ERROR_SHARING_VIOLATION) {
            set_why(why, why_cap, "the port is in use by another program");
        } else if (err == ERROR_SEM_TIMEOUT) {
            /* a Bluetooth port whose adapter is off or out of range */
            set_why(why, why_cap, "the device behind the port didn't answer");
        } else if (why_cap > 0) {
            snprintf(why, why_cap, "the port didn't open (Windows error %lu)",
                     static_cast<unsigned long>(err));
        }
        return ECUIO_ERR_LOAD_FAILED;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        set_why(why, why_cap, "it isn't a serial port");
        CloseHandle(h);
        return ECUIO_ERR_LOAD_FAILED;
    }
    dcb.BaudRate = serial_baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        if (why_cap > 0) {
            snprintf(why, why_cap,
                     "the port refused %lu bit/s, 8N1 (Windows error %lu)",
                     static_cast<unsigned long>(serial_baud),
                     static_cast<unsigned long>(GetLastError()));
        }
        CloseHandle(h);
        return ECUIO_ERR_LOAD_FAILED;
    }
    (void)SetupComm(h, 4096, 4096);
    (void)PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    auto* p = new (std::nothrow) win_port();
    if (p == nullptr) {
        CloseHandle(h);
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    p->handle = h;
    if (!set_read_timeout(p, 0)) {
        set_why(why, why_cap, "the port refused its timeouts");
        win_close(p);
        return ECUIO_ERR_LOAD_FAILED;
    }
    out->ops = &kWinPortOps;
    out->impl = p;
    return ECUIO_OK;
}

} // namespace

#else /* POSIX */

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>

namespace {

struct posix_port {
    int fd = -1;
};

/* Waits up to timeout_ms for events on fd: the revents, 0 on a timeout
 * (or a signal), -1 on an error. */
int wait_for(int fd, short events, uint32_t timeout_ms)
{
    pollfd p{};
    p.fd = fd;
    p.events = events;
    const int r = poll(&p, 1, static_cast<int>(timeout_ms));
    if (r < 0) {
        return (errno == EINTR) ? 0 : -1;
    }
    return (r == 0) ? 0 : p.revents;
}

bool posix_write(void* impl, const uint8_t* data, uint32_t len)
{
    const int fd = static_cast<posix_port*>(impl)->fd;
    while (len > 0) {
        const ssize_t n = write(fd, data, len);
        if (n > 0) {
            data += n;
            len -= static_cast<uint32_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int ev = wait_for(fd, POLLOUT, ELM327_PORT_WRITE_TIMEOUT_MS);
            if (ev <= 0 || (ev & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

int32_t posix_read(void* impl, uint8_t* buf, uint32_t cap, uint32_t timeout_ms)
{
    const int fd = static_cast<posix_port*>(impl)->fd;
    const int ev = wait_for(fd, POLLIN, timeout_ms);
    if (ev < 0) {
        return -1;
    }
    if (ev == 0) {
        return 0;
    }
    if ((ev & POLLIN) == 0) {
        return -1; /* POLLHUP, POLLERR: the device or its link went */
    }
    const ssize_t n = read(fd, buf, cap);
    if (n > 0) {
        return static_cast<int32_t>(n);
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    return -1; /* end of file, or an error: the device went away */
}

void posix_close(void* impl)
{
    auto* p = static_cast<posix_port*>(impl);
    close(p->fd); /* releases the flock too */
    delete p;
}

const ecuio_elm327_port_ops_t kPosixPortOps = {posix_write, posix_read,
                                               posix_close};

/* The termios speed for a rate, or false for one this system lacks. */
bool speed_for(uint32_t rate, speed_t* out)
{
    struct rate_speed {
        uint32_t rate;
        speed_t speed;
    };
    static const rate_speed kSpeeds[] = {
        {1200, B1200},       {2400, B2400},     {4800, B4800},
        {9600, B9600},       {19200, B19200},   {38400, B38400},
        {57600, B57600},     {115200, B115200}, {230400, B230400},
#ifdef B460800
        {460800, B460800},
#endif
#ifdef B500000
        {500000, B500000},
#endif
#ifdef B921600
        {921600, B921600},
#endif
#ifdef B1000000
        {1000000, B1000000},
#endif
#ifdef B2000000
        {2000000, B2000000},
#endif
    };
    for (const rate_speed& r : kSpeeds) {
        if (r.rate == rate) {
            *out = r.speed;
            return true;
        }
    }
    return false;
}

} // namespace

bool elm327_port_name_ok(const char* name, size_t n)
{
    if (n <= 5 || n > ELM327_PORT_NAME_MAX || strncmp(name, "/dev/", 5) != 0) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (static_cast<unsigned char>(name[i]) < 0x20 || name[i] == 0x7F) {
            return false;
        }
    }
    return true;
}

const char* elm327_port_name_example()
{
    return "a /dev/ path, like /dev/ttyUSB0 or /dev/cu.OBDLink";
}

namespace {

ecuio_status_t serial_open(const char* name, uint32_t serial_baud,
                           ecuio_elm327_port_t* out, char* why,
                           uint32_t why_cap)
{
    speed_t speed = B38400;
    if (!speed_for(serial_baud, &speed)) {
        if (why_cap > 0) {
            snprintf(why, why_cap,
                     "this system's serial driver has no %lu bit/s rate",
                     static_cast<unsigned long>(serial_baud));
        }
        return ECUIO_ERR_LOAD_FAILED;
    }
    const int fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            set_why(why, why_cap, "no such port");
        } else if (errno == EACCES) {
            set_why(why, why_cap,
                    "permission denied: the user needs access to the port "
                    "(on Linux, the dialout group)");
        } else if (errno == EBUSY) {
            set_why(why, why_cap, "the port is in use by another program");
        } else if (why_cap > 0) {
            snprintf(why, why_cap, "the port didn't open (%s)",
                     strerror(errno));
        }
        return ECUIO_ERR_LOAD_FAILED;
    }
    /* Another program holding it the same way shows here */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
        set_why(why, why_cap, "the port is in use by another program");
        close(fd);
        return ECUIO_ERR_LOAD_FAILED;
    }
    termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        set_why(why, why_cap, "it isn't a serial port");
        close(fd);
        return ECUIO_ERR_LOAD_FAILED;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);
#ifdef CRTSCTS
    tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS); /* no flow control */
#endif
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0 ||
        tcsetattr(fd, TCSANOW, &tio) != 0) {
        if (why_cap > 0) {
            snprintf(why, why_cap, "the port refused %lu bit/s, 8N1 (%s)",
                     static_cast<unsigned long>(serial_baud), strerror(errno));
        }
        close(fd);
        return ECUIO_ERR_LOAD_FAILED;
    }
    (void)tcflush(fd, TCIOFLUSH);

    auto* p = new (std::nothrow) posix_port();
    if (p == nullptr) {
        close(fd);
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    p->fd = fd;
    out->ops = &kPosixPortOps;
    out->impl = p;
    return ECUIO_OK;
}

} // namespace

#endif /* _WIN32 */

ecuio_status_t elm327_port_open_os(const char* name, uint32_t serial_baud,
                                   ecuio_elm327_port_t* out, char* why,
                                   uint32_t why_cap)
{
    /* A WiFi adapter's address (ADR-0026's appendix "WiFi over TCP") */
    if (elm327_tcp_address(name, strlen(name), nullptr, nullptr)) {
        return elm327_tcp_open(name, out, why, why_cap);
    }
    return serial_open(name, serial_baud, out, why, why_cap);
}
