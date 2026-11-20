// File/layer: tests/test_elm327_session_ports.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327PortTest, Elm327TcpTest, Elm327StreamTest
//          (test_elm327)

#include "test_elm327_support.h"

#ifndef _WIN32

/* ---- The POSIX port over a pseudo-terminal (ADR-0026 section 8) ---------------
 * The fake ELM327 on the master side, the transport's termios port on
 * the slave: the real port code, its settings and its timeouts. */

namespace {

/* A pseudo-terminal pair with the fake on its master, pumped by a
 * thread until destroyed. The slave stays open here too, so the master
 * never reads end of file between the transport's opens. */
class PtyElm327
{
public:
    PtyElm327()
    {
        master_ = posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0 || grantpt(master_) != 0 || unlockpt(master_) != 0) {
            return;
        }
        const char* name = ptsname(master_);
        if (name == nullptr) {
            return;
        }
        slave_name_ = name;
        hold_ = open(name, O_RDWR | O_NOCTTY);
        adapter_.clear();
        pump_ = std::thread([this] { run(); });
    }
    ~PtyElm327()
    {
        stop_ = true;
        if (pump_.joinable()) {
            pump_.join();
        }
        close_master();
        if (hold_ >= 0) {
            close(hold_);
        }
    }
    bool ok() const { return hold_ >= 0 && pump_.joinable(); }
    const std::string& slave() const { return slave_name_; }
    uint32_t requests()
    {
        std::lock_guard<std::mutex> g(lock_);
        return adapter_.requests;
    }
    /* The far side goes away: what a dropped link looks like. */
    void close_master()
    {
        stop_ = true;
        if (pump_.joinable()) {
            pump_.join();
        }
        if (master_ >= 0) {
            close(master_);
            master_ = -1;
        }
    }

private:
    int master_ = -1;
    int hold_ = -1;
    std::string slave_name_;
    std::mutex lock_;
    fake_elm327::adapter adapter_;
    std::atomic<bool> stop_{false};
    std::thread pump_;

    void run()
    {
        uint8_t buf[512];
        while (!stop_.load()) {
            pollfd p{};
            p.fd = master_;
            p.events = POLLIN;
            if (poll(&p, 1, 5) > 0 && (p.revents & POLLIN) != 0) {
                const ssize_t n = read(master_, buf, sizeof(buf));
                if (n > 0) {
                    std::lock_guard<std::mutex> g(lock_);
                    adapter_.from_host(buf, static_cast<size_t>(n),
                                       fake_elm327::steady_us());
                }
            }
            size_t n = 0;
            {
                std::lock_guard<std::mutex> g(lock_);
                n = adapter_.to_host(buf, sizeof(buf),
                                     fake_elm327::steady_us());
            }
            for (size_t done = 0; done < n;) {
                const ssize_t w = write(master_, buf + done, n - done);
                if (w <= 0) {
                    break;
                }
                done += static_cast<size_t>(w);
            }
        }
    }
};

} // namespace

/* A whole session through the OS's port: setup, requests and values */
TEST(Elm327PortTest, ASessionRunsOverAPseudoTerminal)
{
    static PtyElm327 pty; /* 600 KB of fake: off the stack */
    ASSERT_TRUE(pty.ok()) << strerror(errno);
    ecuio_elm327_set_port_opener(nullptr);
    Session s;
    ASSERT_EQ(s.create("elm327://" + pty.slave() + "?poll=0x0C&serial=115200"),
              ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    EXPECT_TRUE(
        wait_for([&] { return s.cap.count("41 0C 1F 40") >= 3; }, 5000));
    EXPECT_EQ(s.state(), ECUIO_OK);
    EXPECT_GE(pty.requests(), 3u);
}

/* The port's reads: nothing within the timeout returns 0 after about
 * that long; the far side closing is a failure, not a timeout */
TEST(Elm327PortTest, ReadsTimeOutAndSeeTheFarSideGo)
{
    static PtyElm327 pty;
    ASSERT_TRUE(pty.ok()) << strerror(errno);
    ecuio_elm327_port_t port{};
    char why[128] = "";
    ASSERT_EQ(elm327_port_open_os(pty.slave().c_str(), 38400, &port, why,
                                  sizeof(why)),
              ECUIO_OK)
        << why;
    uint8_t buf[64];
    const auto from = std::chrono::steady_clock::now();
    EXPECT_EQ(port.ops->read(port.impl, buf, sizeof(buf), 80), 0);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - from)
                          .count();
    EXPECT_GE(took, 70);
    EXPECT_LT(took, 1000);

    ASSERT_TRUE(port.ops->write(port.impl,
                                reinterpret_cast<const uint8_t*>("ATI\r"), 4));
    std::string got;
    ASSERT_TRUE(wait_for(
        [&] {
            const int32_t n = port.ops->read(port.impl, buf, sizeof(buf), 20);
            if (n > 0) {
                got.append(reinterpret_cast<const char*>(buf),
                           static_cast<size_t>(n));
            }
            return got.find('>') != std::string::npos;
        },
        2000));
    EXPECT_NE(got.find("ELM327 v1.4b"), std::string::npos) << got;

    pty.close_master();
    EXPECT_TRUE(wait_for(
        [&] { return port.ops->read(port.impl, buf, sizeof(buf), 20) < 0; },
        2000))
        << "a far side that went is a failed port";
    port.ops->close(port.impl);
}

/* What the OS's opener says for a port that isn't there */
TEST(Elm327PortTest, AMissingPortIsNoSuchPort)
{
    ecuio_elm327_port_t port{};
    char why[128] = "";
    EXPECT_EQ(elm327_port_open_os("/dev/havne-no-such-port", 38400, &port, why,
                                  sizeof(why)),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(why, "no such port");
}

#endif /* !_WIN32 */

namespace {

/* A send to a transport that just closed is EPIPE, never a SIGPIPE that
 * ends the test: MSG_NOSIGNAL on Linux, SO_NOSIGPIPE on macOS */
#ifdef MSG_NOSIGNAL
const int kSendFlags = MSG_NOSIGNAL;
#else
const int kSendFlags = 0;
#endif

/* Winsock for the test's own sockets; nothing elsewhere */
class SocketsUp
{
public:
    SocketsUp()
    {
#ifdef _WIN32
        WSADATA w{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &w) == 0;
#endif
    }
    ~SocketsUp()
    {
#ifdef _WIN32
        if (ok_) {
            WSACleanup();
        }
#endif
    }
    SocketsUp(const SocketsUp&) = delete;
    SocketsUp& operator=(const SocketsUp&) = delete;
    bool ok() const { return ok_; }

private:
    bool ok_ = true;
};

/* A listener on 127.0.0.1 at a port the OS picks, bound and closed: the
 * port, with nothing listening on it */
uint16_t a_closed_port()
{
    const test_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    uint16_t port = 0;
    if (bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) {
        port = ntohs(a.sin_port);
    }
    close_sock(s);
    return port;
}

/* The fake ELM327 behind a loopback listener, one connection at a time,
 * pumped by a thread until destroyed. A transport that reconnects is
 * accepted again; the adapter keeps its state, as the VX does. */
class TcpElm327
{
public:
    TcpElm327()
    {
        if (!sockets_.ok()) {
            return;
        }
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        if (listen_ == kNoSock ||
            bind(listen_, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) !=
                0 ||
            listen(listen_, 1) != 0 ||
            getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
            return;
        }
        port_ = ntohs(a.sin_port);
        adapter_.clear();
        pump_ = std::thread([this] { run(); });
    }
    ~TcpElm327()
    {
        stop_ = true;
        if (pump_.joinable()) {
            pump_.join();
        }
        if (conn_ != kNoSock) {
            close_sock(conn_);
        }
        if (listen_ != kNoSock) {
            close_sock(listen_);
        }
    }
    TcpElm327(const TcpElm327&) = delete;
    TcpElm327& operator=(const TcpElm327&) = delete;

    bool ok() const { return port_ != 0 && pump_.joinable(); }
    std::string address() const { return "127.0.0.1:" + std::to_string(port_); }
    uint32_t requests()
    {
        std::lock_guard<std::mutex> g(lock_);
        return adapter_.requests;
    }
    uint32_t accepted() const { return accepted_.load(); }
    /* The far side closes the connection: what leaving the adapter's
     * network looks like. The listener stays, so a reconnect gets in. */
    void drop() { drop_ = true; }

private:
    SocketsUp sockets_;
    test_sock listen_ = kNoSock;
    test_sock conn_ = kNoSock; /* the pump thread's alone */
    uint16_t port_ = 0;
    std::mutex lock_;
    fake_elm327::adapter adapter_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> drop_{false};
    std::atomic<uint32_t> accepted_{0};
    std::thread pump_;

    void hang_up()
    {
        close_sock(conn_);
        conn_ = kNoSock;
    }

    void run()
    {
        uint8_t buf[512];
        while (!stop_.load()) {
            if (conn_ == kNoSock) {
                if (readable(listen_, 5)) {
                    conn_ = accept(listen_, nullptr, nullptr);
                    if (conn_ != kNoSock) {
#ifdef SO_NOSIGPIPE
                        const int one = 1;
                        setsockopt(conn_, SOL_SOCKET, SO_NOSIGPIPE, &one,
                                   sizeof(one));
#endif
                        accepted_.fetch_add(1);
                    }
                }
                continue;
            }
            if (drop_.exchange(false)) {
                hang_up();
                continue;
            }
            if (readable(conn_, 5)) {
                const auto n = recv(conn_, reinterpret_cast<char*>(buf),
                                    static_cast<int>(sizeof(buf)), 0);
                if (n <= 0) {
                    hang_up(); /* the transport closed its side */
                    continue;
                }
                std::lock_guard<std::mutex> g(lock_);
                adapter_.from_host(buf, static_cast<size_t>(n),
                                   fake_elm327::steady_us());
            }
            size_t n = 0;
            {
                std::lock_guard<std::mutex> g(lock_);
                n = adapter_.to_host(buf, sizeof(buf),
                                     fake_elm327::steady_us());
            }
            for (size_t done = 0; done < n;) {
                const auto w =
                    send(conn_, reinterpret_cast<const char*>(buf + done),
                         static_cast<int>(n - done), kSendFlags);
                if (w <= 0) {
                    break;
                }
                done += static_cast<size_t>(w);
            }
        }
    }
};

} // namespace

/* A whole session through the OS's opener and a real socket: setup,
 * requests and values, and the wire log says what it runs over */
TEST(Elm327TcpTest, ASessionRunsOverALoopbackSocket)
{
    /* 600 KB of fake: off the stack */
    auto srv = std::make_unique<TcpElm327>();
    ASSERT_TRUE(srv->ok());
    ecuio_elm327_set_port_opener(nullptr);
    LogDir dir("tcp-session");
    {
        Session s;
        ASSERT_EQ(s.create("elm327://" + srv->address() + "?poll=0x0C"),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        s.start();
        EXPECT_TRUE(
            wait_for([&] { return s.cap.count("41 0C 1F 40") >= 3; }, 5000));
        EXPECT_EQ(s.state(), ECUIO_OK);
        EXPECT_GE(srv->requests(), 3u);
    }
    EXPECT_TRUE(in_order(dir.only_log(),
                         {srv->address() + " over TCP; CAN at 500000 bit/s"}));
}

/* The far side closing is a failed read: a hard failure, the socket
 * connected again on the backoff and the setup run, with no reconnect */
TEST(Elm327TcpTest, TheFarSideClosingIsAHardFailureThatRecovers)
{
    auto srv = std::make_unique<TcpElm327>();
    ASSERT_TRUE(srv->ok());
    ecuio_elm327_set_port_opener(nullptr);
    Session s;
    ASSERT_EQ(s.create("elm327://" + srv->address() + "?poll=0x0C"), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(
        wait_for([&] { return s.cap.count("41 0C 1F 40") >= 2; }, 5000));
    EXPECT_EQ(srv->accepted(), 1u);

    srv->drop();
    uint32_t episodes = 0;
    EXPECT_TRUE(wait_for(
        [&] {
            return s.state(&episodes) == ECUIO_ERR_TRANSPORT_HARD_FAILURE ||
                   episodes > 0;
        },
        5000))
        << "the close is a hard failure";
    const size_t before = s.cap.count("41 0C 1F 40");
    EXPECT_TRUE(wait_for(
        [&] {
            return srv->accepted() == 2u && s.state() == ECUIO_OK &&
                   s.cap.count("41 0C 1F 40") > before;
        },
        5000))
        << "connected again, values again";
    s.state(&episodes);
    EXPECT_EQ(episodes, 1u);
}

/* The port's reads over a socket: nothing within the timeout returns 0
 * after about that long; the far side closing is a failure, not a
 * timeout */
TEST(Elm327TcpTest, ReadsTimeOutAndSeeTheFarSideGo)
{
    auto srv = std::make_unique<TcpElm327>();
    ASSERT_TRUE(srv->ok());
    ecuio_elm327_port_t port{};
    char why[128] = "";
    ASSERT_EQ(elm327_port_open_os(srv->address().c_str(), 38400, &port, why,
                                  sizeof(why)),
              ECUIO_OK)
        << why;
    uint8_t buf[64];
    const auto from = std::chrono::steady_clock::now();
    EXPECT_EQ(port.ops->read(port.impl, buf, sizeof(buf), 80), 0);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - from)
                          .count();
    EXPECT_GE(took, 70);
    EXPECT_LT(took, 1000);

    ASSERT_TRUE(port.ops->write(port.impl,
                                reinterpret_cast<const uint8_t*>("ATI\r"), 4));
    std::string got;
    ASSERT_TRUE(wait_for(
        [&] {
            const int32_t n = port.ops->read(port.impl, buf, sizeof(buf), 20);
            if (n > 0) {
                got.append(reinterpret_cast<const char*>(buf),
                           static_cast<size_t>(n));
            }
            return got.find('>') != std::string::npos;
        },
        2000));
    EXPECT_NE(got.find("ELM327 v1.4b"), std::string::npos) << got;

    srv->drop();
    EXPECT_TRUE(wait_for(
        [&] { return port.ops->read(port.impl, buf, sizeof(buf), 20) < 0; },
        2000))
        << "a far side that went is a failed port";
    port.ops->close(port.impl);
}

/* Nothing listening is a refusal, said as one and within the connect's
 * limit (Windows' SYN retries cut to two, ADR-0026's appendix) */
TEST(Elm327TcpTest, NothingListeningIsARefusal)
{
    SocketsUp up;
    ASSERT_TRUE(up.ok());
    const uint16_t port = a_closed_port();
    ASSERT_NE(port, 0u);
    ecuio_elm327_set_port_opener(nullptr);
    const std::string address = "127.0.0.1:" + std::to_string(port);
    ecuio_session_t* s = nullptr;
    const auto from = std::chrono::steady_clock::now();
    EXPECT_EQ(ecuio_session_create(("elm327://" + address).c_str(), &s),
              ECUIO_ERR_LOAD_FAILED);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - from)
                          .count();
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              address + ": the connection was refused");
    EXPECT_LT(took, static_cast<long long>(ELM327_TCP_CONNECT_MS));
    EXPECT_EQ(s, nullptr);
}

/* A whole session through the app's stream: setup, requests and values;
 * the wire log names the link; the close reaches the app, and the stream
 * is the app's to unregister once the session is gone */
TEST(Elm327StreamTest, ASessionRunsThroughTheAppsStream)
{
    /* 600 KB of fake: off the stack */
    auto app = std::make_unique<AppStream>(kBt);
    ASSERT_EQ(app->registered(), ECUIO_OK);
    LogDir dir("stream-session");
    {
        Session s;
        ASSERT_EQ(s.create(bt_uri("?poll=0x0C")), ECUIO_OK)
            << ecuio_last_error_detail();
        s.start();
        EXPECT_TRUE(
            wait_for([&] { return s.cap.count("41 0C 1F 40") >= 3; }, 5000));
        EXPECT_EQ(s.state(), ECUIO_OK);
        EXPECT_GE(app->requests(), 3u);
        EXPECT_EQ(app->unregister(), ECUIO_ERR_INVALID_ARGUMENT)
            << "refused while a session holds it";
        EXPECT_EQ(app->opens(), 1u);
        EXPECT_EQ(app->closes(), 0u);
    }
    EXPECT_EQ(app->closes(), 1u);
    EXPECT_EQ(app->unregister(), ECUIO_OK);
    EXPECT_TRUE(
        in_order(dir.only_log(),
                 {std::string(kBt) + " (app stream); CAN at 500000 bit/s"}));
}
