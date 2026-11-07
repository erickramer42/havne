// File/layer: cli/elm327_listener.h | havne-cli, the ELM327 emulator's
//             TCP listener (ADR-0055); public headers only
// Testing: HARNESS: CliElm327RunTest (tests/cli_elm327.py); a failed wait
//          by hand
//
// Serves cli/elm327_emulator.h on a TCP port, as a WiFi ELM327 adapter
// does, at the address cli/elm327_lines.h parsed. One host at a time, as
// an adapter takes one: another connection is closed at once (a `client`
// line says so). Each connection is a power on: the settings start from
// their defaults. Kept out of the pure headers: the socket headers bring
// the platform's macros.

#ifndef HAVNE_CLI_ELM327_LISTENER_H
#define HAVNE_CLI_ELM327_LISTENER_H

#include "elm327_emulator.h"
#include "elm327_lines.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
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

namespace cli {

/* The listener and its one host, served through an Elm327Emulator */
class Elm327Server
{
public:
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket kNone = INVALID_SOCKET;
#else
    using Socket = int;
    static constexpr Socket kNone = -1;
#endif

    Elm327Server() = default;
    Elm327Server(const Elm327Server&) = delete;
    Elm327Server& operator=(const Elm327Server&) = delete;
    ~Elm327Server()
    {
        drop(client_);
        drop(listener_);
#ifdef _WIN32
        if (started_) {
            WSACleanup();
        }
#endif
    }

    /* Listens on a; false with why (one line) */
    bool open(const Elm327Address& a, std::string* why)
    {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            *why = "Windows sockets wouldn't start";
            return false;
        }
        started_ = true;
        listener_ = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                               WSA_FLAG_NO_HANDLE_INHERIT);
#else
        listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
        if (listener_ == kNone) {
            *why = "the socket wouldn't open";
            return false;
        }
        int one = 1;
#ifdef _WIN32
        /* no other socket may take the port beside it */
        (void)setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                         reinterpret_cast<const char*>(&one), sizeof one);
#else
        /* a restart takes the port while old connections time out */
        (void)setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(a.ip);
        addr.sin_port = htons(a.port);
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&addr),
                 sizeof addr) != 0) {
            *why = address_text(a.ip, a.port) +
                   " wouldn't take a listener (the port is in use, or the "
                   "address isn't this computer's)";
            return false;
        }
        if (listen(listener_, 4) != 0) {
            *why = "the socket wouldn't listen";
            return false;
        }
        sockaddr_in got{};
        socklen_t len = sizeof got;
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&got), &len) !=
            0) {
            *why = "the socket's port couldn't be read";
            return false;
        }
        address_ = a;
        address_.port = ntohs(got.sin_port);
        return true;
    }

    const Elm327Address& address() const { return address_; }

    /* Why serve() returned before stop() said so (its wait failed, in
     * the system's words), or "" */
    const std::string& failure() const { return failure_; }

    /* Serves until stop() says so, waking at least every poll_ms: a host
     * at a time, its bytes to emu, emu's bytes back when due (clock()
     * is emu's time); each connection and its end a line through
     * emit */
    void serve(Elm327Emulator& emu, const std::function<int64_t()>& clock,
               const std::function<bool()>& stop,
               const std::function<void(const char*, const std::string&)>& emit,
               int poll_ms = 100)
    {
        uint8_t buf[4096];
        while (!stop()) {
            const int64_t now = clock();
            /* what's due goes out first */
            if (client_ != kNone) {
                for (;;) {
                    const size_t n = emu.to_host(buf, sizeof buf, now);
                    if (n == 0) {
                        break;
                    }
                    if (!send_all(client_, buf, n)) {
                        emit("closed", peer_);
                        drop(client_);
                        break;
                    }
                }
            }
            int wait_ms = poll_ms;
            const int64_t due = emu.next_due_us();
            if (client_ != kNone && due != INT64_MAX) {
                const int64_t until = due - clock();
                wait_ms = until <= 0 ? 0
                                     : static_cast<int>(std::min<int64_t>(
                                           (until + 999) / 1000, poll_ms));
            }
            bool listener_ready = false;
            bool client_ready = false;
            if (!wait(wait_ms, &listener_ready, &client_ready)) {
                return;
            }
            if (listener_ready) {
                take_connection(emu, emit);
            }
            if (client_ready && client_ != kNone) {
                const long got = receive(client_, buf, sizeof buf);
                if (got <= 0) {
                    emit("closed", peer_);
                    drop(client_);
                } else {
                    emu.from_host(buf, static_cast<size_t>(got), clock());
                }
            }
        }
    }

private:
    Socket listener_ = kNone;
    Socket client_ = kNone;
    std::string peer_;
    Elm327Address address_;
    std::string failure_;
#ifdef _WIN32
    bool started_ = false;
#endif

    static void drop(Socket& s)
    {
        if (s == kNone) {
            return;
        }
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        s = kNone;
    }

    static std::string peer_of(const sockaddr_in& a)
    {
        return address_text(ntohl(a.sin_addr.s_addr), ntohs(a.sin_port));
    }

    /* Waits up to ms for the listener or the host; false when the wait
     * itself failed */
    bool wait(int ms, bool* listener_ready, bool* client_ready)
    {
#ifdef _WIN32
        fd_set r;
        FD_ZERO(&r);
        FD_SET(listener_, &r);
        if (client_ != kNone) {
            FD_SET(client_, &r);
        }
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        const int got = select(0, &r, nullptr, nullptr, &tv);
        if (got == SOCKET_ERROR) {
            failure_ = "the listener's wait failed: " +
                       std::system_category().message(WSAGetLastError());
            return false;
        }
        *listener_ready = FD_ISSET(listener_, &r) != 0;
        *client_ready = client_ != kNone && FD_ISSET(client_, &r) != 0;
#else
        pollfd p[2] = {{listener_, POLLIN, 0}, {client_, POLLIN, 0}};
        const int got = poll(p, client_ != kNone ? 2 : 1, ms);
        if (got < 0) {
            if (errno == EINTR) {
                return true;
            }
            failure_ = "the listener's wait failed: " +
                       std::generic_category().message(errno);
            return false;
        }
        *listener_ready = (p[0].revents & POLLIN) != 0;
        *client_ready = client_ != kNone && p[1].revents != 0;
#endif
        return true;
    }

    void take_connection(
        Elm327Emulator& emu,
        const std::function<void(const char*, const std::string&)>& emit)
    {
        sockaddr_in from{};
        socklen_t len = sizeof from;
        Socket s = accept(listener_, reinterpret_cast<sockaddr*>(&from), &len);
        if (s == kNone) {
            return;
        }
        if (client_ != kNone) {
            emit("refused", peer_of(from));
            drop(s);
            return;
        }
        int one = 1;
        (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&one), sizeof one);
#ifdef _WIN32
        DWORD ms = 2000;
        (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                         reinterpret_cast<const char*>(&ms), sizeof ms);
#else
        timeval tv{2, 0};
        (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
        (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
        client_ = s;
        peer_ = peer_of(from);
        emu.power_on();
        emit("connected", peer_);
    }

    static long receive(Socket s, uint8_t* buf, size_t cap)
    {
#ifdef _WIN32
        return recv(s, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0);
#else
        ssize_t n = 0;
        do {
            n = recv(s, buf, cap, 0);
        } while (n < 0 && errno == EINTR);
        return static_cast<long>(n);
#endif
    }

    /* Sends it all; false when the host is gone or stopped reading
     * (the 2 s send timeout) */
    static bool send_all(Socket s, const uint8_t* data, size_t n)
    {
        while (n > 0) {
#ifdef _WIN32
            const int sent = send(s, reinterpret_cast<const char*>(data),
                                  static_cast<int>(n), 0);
#else
#ifdef MSG_NOSIGNAL
            const ssize_t sent = send(s, data, n, MSG_NOSIGNAL);
#else
            const ssize_t sent = send(s, data, n, 0);
#endif
#endif
            if (sent <= 0) {
                return false;
            }
            data += sent;
            n -= static_cast<size_t>(sent);
        }
        return true;
    }
};

} // namespace cli

#endif // HAVNE_CLI_ELM327_LISTENER_H
