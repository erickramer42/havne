// File/layer: core/src/elm327_tcp.cpp | C++17, Qt-free; Winsock and POSIX
//             sockets
// Testing: UNIT — Elm327TcpTest.AddressesAreADottedIpv4AndAPort and
//          .ConnectFailuresAreWorded (test_elm327); HARNESS — Elm327TcpTest (test_elm327_session,
//          every platform): a session over loopback, reads and their
//          timeouts, the far side closing, a refusal; BENCH — the VX's
//          WiFi (ADR-0026's appendix "WiFi over TCP")

#include "elm327_tcp.h"

#include <cstdio>
#include <cstring>
#include <new>

#ifdef _WIN32
#include <winsock2.h> /* before anything that includes <windows.h> */
#include <ws2tcpip.h>
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

/* A send that can't finish in this long has failed: the link stopped
 * taking bytes (elm327_port.cpp's serial write has the same limit). */
constexpr uint32_t ELM327_TCP_WRITE_TIMEOUT_MS = 2000;

#ifdef _WIN32
using sock_t = SOCKET;
const sock_t kNoSocket = INVALID_SOCKET;
#else
using sock_t = int;
const sock_t kNoSocket = -1;
#endif

struct tcp_port {
    sock_t s = kNoSocket;
};

void set_why(char* why, uint32_t cap, const char* text)
{
    if (cap > 0) {
        snprintf(why, cap, "%s", text);
    }
}

/* Digits [p, end) as a decimal number up to max, with no leading zero. */
bool decimal(const char* p, const char* end, unsigned long max,
             unsigned long* out)
{
    if (p == end || end - p > 5 || (*p == '0' && end - p > 1)) {
        return false;
    }
    unsigned long v = 0;
    for (; p < end; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        v = v * 10 + static_cast<unsigned long>(*p - '0');
    }
    if (v > max) {
        return false;
    }
    *out = v;
    return true;
}

int last_error()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

/* The call would block, or was interrupted: try again once ready. */
bool try_again(int e)
{
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == EINTR;
#endif
}

/* A non-blocking connect under way. POSIX lets an interrupted one go on
 * by itself. */
bool connecting(int e)
{
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EINPROGRESS || e == EINTR;
#endif
}

void close_socket(sock_t s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

const char kNoAnswer[] =
    "no answer within 2 s (is this computer on the adapter's WiFi?)";
/* This computer's network stack dropped the connection: on the bench, a
 * reconnect after the adapter's WiFi went away */
const char kCutOff[] = "the connection was cut off on this computer's side "
                       "(did it leave the adapter's WiFi?)";
const char kReset[] = "the adapter reset the connection";

/* A failed connect's reason, in the port's words. */
void describe(int e, char* why, uint32_t cap)
{
#ifdef _WIN32
    if (e == WSAECONNREFUSED) {
        set_why(why, cap, "the connection was refused");
    } else if (e == WSAENETUNREACH || e == WSAENETDOWN) {
        set_why(why, cap, "the network is unreachable");
    } else if (e == WSAEHOSTUNREACH) {
        set_why(why, cap, "the address is unreachable");
    } else if (e == WSAETIMEDOUT) {
        set_why(why, cap, kNoAnswer);
    } else if (e == WSAECONNABORTED) {
        set_why(why, cap, kCutOff);
    } else if (e == WSAECONNRESET) {
        set_why(why, cap, kReset);
    } else if (cap > 0) {
        snprintf(why, cap, "the connection failed (Windows sockets error %d)",
                 e);
    }
#else
    if (e == ECONNREFUSED) {
        set_why(why, cap, "the connection was refused");
    } else if (e == ENETUNREACH || e == ENETDOWN) {
        set_why(why, cap, "the network is unreachable");
    } else if (e == EHOSTUNREACH) {
        set_why(why, cap, "the address is unreachable");
    } else if (e == ETIMEDOUT) {
        set_why(why, cap, kNoAnswer);
    } else if (e == ECONNABORTED) {
        set_why(why, cap, kCutOff);
    } else if (e == ECONNRESET) {
        set_why(why, cap, kReset);
    } else if (cap > 0) {
        snprintf(why, cap, "the connection failed (%s)", strerror(e));
    }
#endif
}

enum class readiness { ready, timeout, failed };

/* Waits up to timeout_ms for s to take a write (for_write) or to have
 * something to read: data, the far side's close, or an error, which the
 * next call reports. */
readiness wait_on(sock_t s, bool for_write, uint32_t timeout_ms)
{
#ifdef _WIN32
    /* select, not WSAPoll, which older Windows let miss a failed
     * connect; select reports one in the except set */
    fd_set want;
    fd_set err;
    FD_ZERO(&want);
    FD_ZERO(&err);
    FD_SET(s, &want);
    FD_SET(s, &err);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout_ms / 1000);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
    const int r = select(0, for_write ? nullptr : &want,
                         for_write ? &want : nullptr, &err, &tv);
    if (r < 0) {
        return readiness::failed;
    }
    if (r == 0) {
        return readiness::timeout;
    }
    return FD_ISSET(s, &want) ? readiness::ready : readiness::failed;
#else
    pollfd p{};
    p.fd = s;
    p.events = for_write ? POLLOUT : POLLIN;
    const int r = poll(&p, 1, static_cast<int>(timeout_ms));
    if (r < 0) {
        return (errno == EINTR) ? readiness::timeout : readiness::failed;
    }
    if (r == 0) {
        return readiness::timeout;
    }
    /* POLLIN with POLLHUP: the far side closed, and the read sees it */
    return ((p.revents & p.events) != 0) ? readiness::ready : readiness::failed;
#endif
}

bool tcp_write(void* impl, const uint8_t* data, uint32_t len)
{
    const sock_t s = static_cast<tcp_port*>(impl)->s;
    while (len > 0) {
#ifdef _WIN32
        const int n = send(s, reinterpret_cast<const char*>(data),
                           static_cast<int>(len), 0);
#else
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL; /* Linux: a closed socket is EPIPE, no SIGPIPE */
#endif
        const ssize_t n = send(s, data, len, flags);
#endif
        if (n > 0) {
            data += n;
            len -= static_cast<uint32_t>(n);
            continue;
        }
        if (n < 0 && try_again(last_error())) {
            if (wait_on(s, true, ELM327_TCP_WRITE_TIMEOUT_MS) !=
                readiness::ready) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

int32_t tcp_read(void* impl, uint8_t* buf, uint32_t cap, uint32_t timeout_ms)
{
    const sock_t s = static_cast<tcp_port*>(impl)->s;
    const readiness r = wait_on(s, false, timeout_ms);
    if (r == readiness::timeout) {
        return 0;
    }
    if (r == readiness::failed) {
        return -1;
    }
#ifdef _WIN32
    const int n =
        recv(s, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0);
#else
    const ssize_t n = recv(s, buf, cap, 0);
#endif
    if (n > 0) {
        return static_cast<int32_t>(n);
    }
    if (n == 0) {
        return -1; /* the far side closed: the link is gone */
    }
    return try_again(last_error()) ? 0 : -1;
}

void tcp_close(void* impl)
{
    auto* p = static_cast<tcp_port*>(impl);
    if (p->s != kNoSocket) {
        close_socket(p->s);
    }
    delete p;
#ifdef _WIN32
    WSACleanup(); /* the open's WSAStartup; both count */
#endif
}

const ecuio_elm327_port_ops_t kTcpPortOps = {tcp_write, tcp_read, tcp_close};

/* A socket that never blocks and never passes to a child process, Nagle
 * off; false when it can't be made non-blocking. */
bool open_socket(sock_t* out)
{
#ifdef _WIN32
    /* No inheritance: the J2534 surrogate is a child process */
    const sock_t s =
        WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                   WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == kNoSocket) {
        return false;
    }
    *out = s;
    u_long on = 1;
    if (ioctlsocket(s, FIONBIO, &on) != 0) {
        return false;
    }
#if defined(SIO_TCP_INITIAL_RTO) && defined(TCP_INITIAL_RTO_UNSPECIFIED_RTT)
    /* Windows answers a refusal by sending the SYN again, every 500 ms,
     * two seconds in all by default, which a 2 s limit would read as no
     * answer. Two retries report it in about a second and still resend a
     * SYN that WiFi lost. */
    TCP_INITIAL_RTO_PARAMETERS rto{};
    rto.Rtt = TCP_INITIAL_RTO_UNSPECIFIED_RTT;
    rto.MaxSynRetransmissions = 2;
    DWORD unused = 0;
    (void)WSAIoctl(s, SIO_TCP_INITIAL_RTO, &rto, sizeof(rto), nullptr, 0,
                   &unused, nullptr, nullptr);
#endif
#else
    const sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        return false;
    }
    *out = s;
    const int fl = fcntl(s, F_GETFL);
    if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) != 0 ||
        fcntl(s, F_SETFD, FD_CLOEXEC) != 0) {
        return false;
    }
#ifdef SO_NOSIGPIPE
    /* macOS: a closed socket is EPIPE, no SIGPIPE */
    const int nosig = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
#endif
    /* Each command is a few bytes and then a wait for its prompt:
     * nothing gains from coalescing, and a held write delays a request */
    const int nodelay = 1;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    return true;
}

ecuio_status_t connect_within(sock_t s, uint32_t ip, uint16_t port, char* why,
                              uint32_t why_cap)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    if (connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0) {
        return ECUIO_OK;
    }
    const int e = last_error();
    if (!connecting(e)) {
        describe(e, why, why_cap);
        return ECUIO_ERR_LOAD_FAILED;
    }
    if (wait_on(s, true, ELM327_TCP_CONNECT_MS) == readiness::timeout) {
        set_why(why, why_cap, kNoAnswer);
        return ECUIO_ERR_LOAD_FAILED;
    }
    /* Ready or failed, the socket's own error says which */
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err),
                   &len) != 0) {
        err = last_error();
    }
    if (err != 0) {
        describe(err, why, why_cap);
        return ECUIO_ERR_LOAD_FAILED;
    }
    return ECUIO_OK;
}

} // namespace

bool elm327_tcp_address(const char* name, size_t n, uint32_t* ip,
                        uint16_t* port)
{
    const char* p = name;
    const char* end = name + n;
    uint32_t addr = 0;
    for (int i = 0; i < 4; ++i) {
        const char sep = (i < 3) ? '.' : ':';
        const char* q = p;
        while (q < end && *q != sep) {
            ++q;
        }
        unsigned long v = 0;
        if (q == end || !decimal(p, q, 255, &v)) {
            return false;
        }
        addr = (addr << 8) | static_cast<uint32_t>(v);
        p = q + 1;
    }
    unsigned long v = 0;
    if (!decimal(p, end, 65535, &v) || v == 0) {
        return false;
    }
    if (ip != nullptr) {
        *ip = addr;
    }
    if (port != nullptr) {
        *port = static_cast<uint16_t>(v);
    }
    return true;
}

const char* elm327_tcp_example()
{
    return "an IPv4 address and port, like 192.168.0.10:35000";
}

void elm327_tcp_describe(int error, char* why, uint32_t why_cap)
{
    describe(error, why, why_cap);
}

ecuio_status_t elm327_tcp_open(const char* name, ecuio_elm327_port_t* out,
                               char* why, uint32_t why_cap)
{
    uint32_t ip = 0;
    uint16_t port = 0;
    if (!elm327_tcp_address(name, strlen(name), &ip, &port)) {
        set_why(why, why_cap, "it isn't an IPv4 address and port");
        return ECUIO_ERR_LOAD_FAILED;
    }
#ifdef _WIN32
    WSADATA wsa{};
    const int started = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (started != 0) {
        if (why_cap > 0) {
            snprintf(why, why_cap, "Windows sockets didn't start (error %d)",
                     started);
        }
        return ECUIO_ERR_LOAD_FAILED;
    }
#endif
    auto* p = new (std::nothrow) tcp_port();
    if (p == nullptr) {
#ifdef _WIN32
        WSACleanup();
#endif
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (!open_socket(&p->s)) {
        if (why_cap > 0) {
            snprintf(why, why_cap, "the socket didn't open (error %d)",
                     last_error());
        }
        tcp_close(p);
        return ECUIO_ERR_LOAD_FAILED;
    }
    const ecuio_status_t st = connect_within(p->s, ip, port, why, why_cap);
    if (st != ECUIO_OK) {
        tcp_close(p);
        return st;
    }
    out->ops = &kTcpPortOps;
    out->impl = p;
    return ECUIO_OK;
}
