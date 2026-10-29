// File/layer: tests/vcan_bus.h | test-only, C++17, Qt-free, Linux only
//             (SocketCAN); header-only
// Testing: HARNESS, the simulated bus for SocketCAN: fake_ecu.h's engine
//          ECU answering on a virtual CAN interface through the kernel's
//          ISO-TP, and a recording's replies played onto it.
//          test_vcan.cpp checks it on its own
//
// The kernel does the ISO 15765-2 framing (the can-isotp module): Single
// Frames, First and Consecutive Frames and flow control, so what a
// listener on the bus sees is what an ECU puts there, padded to 8 bytes.
// Needs the interface up (vcan0 unless ECUIO_VCAN_IF names another):
//   sudo modprobe vcan can-isotp
//   sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
// available() says whether it is, and why not.

#ifndef ECUIO_TESTS_VCAN_BUS_H
#define ECUIO_TESTS_VCAN_BUS_H

#include "fake_ecu.h"

#include <linux/can.h>
#include <linux/can/isotp.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace vcan_bus {

/* The engine ECU's addresses (ISO 15765-4, 11-bit) */
constexpr uint32_t kFunctional = 0x7DF;
constexpr uint32_t kRequest = 0x7E0;
constexpr uint32_t kReply = 0x7E8;
/* What the kernel pads a frame's unused bytes with */
constexpr uint8_t kPad = 0x00;

inline const char* interface_name()
{
    const char* name = std::getenv("ECUIO_VCAN_IF");
    return (name != nullptr && name[0] != '\0') ? name : "vcan0";
}

/* An owned socket, closed when it goes */
class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Fd& operator=(Fd&& o) noexcept
    {
        if (this != &o) {
            reset();
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }
    int get() const { return fd_; }
    bool ok() const { return fd_ >= 0; }
    void reset()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = -1;
    }

private:
    int fd_;
};

/* A frame's ID as SocketCAN carries it: 29-bit IDs need the flag */
inline canid_t can_id(uint32_t id)
{
    return (id > CAN_SFF_MASK) ? (id | CAN_EFF_FLAG) : id;
}

/* A raw socket on the interface: every frame on the bus, our own
 * writes from other sockets included. Invalid when the interface
 * isn't there. */
inline Fd open_raw(const char* ifname = interface_name())
{
    const unsigned index = if_nametoindex(ifname);
    if (index == 0) {
        return Fd();
    }
    Fd s(::socket(PF_CAN, SOCK_RAW, CAN_RAW));
    if (!s.ok()) {
        return s;
    }
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(index);
    if (::bind(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        s.reset();
    }
    return s;
}

/* An ISO-TP socket that sends on tx and receives on rx (and expects
 * the flow control for its long messages on rx), padding its frames */
inline Fd open_isotp(uint32_t tx, uint32_t rx, const char* ifname = interface_name())
{
    const unsigned index = if_nametoindex(ifname);
    if (index == 0) {
        return Fd();
    }
    Fd s(::socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP));
    if (!s.ok()) {
        return s;
    }
    can_isotp_options opts{};
    opts.flags = CAN_ISOTP_TX_PADDING;
    opts.txpad_content = kPad;
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(index);
    addr.can_addr.tp.tx_id = can_id(tx);
    addr.can_addr.tp.rx_id = can_id(rx);
    if (::setsockopt(s.get(), SOL_CAN_ISOTP, CAN_ISOTP_OPTS, &opts, sizeof(opts)) != 0
        || ::bind(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        s.reset();
    }
    return s;
}

/* Whether the bus can be used: the interface is up and ISO-TP sockets
 * open. why says what's missing. */
inline bool available(std::string* why)
{
    const char* name = interface_name();
    if (if_nametoindex(name) == 0) {
        *why = std::string("no ") + name + " (sudo modprobe vcan; sudo ip link add dev " + name
               + " type vcan; sudo ip link set up " + name + ")";
        return false;
    }
    if (!open_raw(name).ok()) {
        *why = std::string("a raw CAN socket on ") + name + " won't open: " + std::strerror(errno);
        return false;
    }
    if (!open_isotp(kRequest, kReply, name).ok()) {
        *why = std::string("an ISO-TP socket won't open (sudo modprobe can-isotp): ")
               + std::strerror(errno);
        return false;
    }
    return true;
}

/* Waits up to timeout_ms for fd to be readable, then reads one message
 * (a frame on a raw socket, a whole message on an ISO-TP one). Returns
 * its length, or -1 on a timeout or an error. */
inline long read_for(int fd, void* buf, size_t cap, int timeout_ms)
{
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, timeout_ms) != 1) {
        return -1;
    }
    return static_cast<long>(::read(fd, buf, cap));
}

/* The engine ECU on the bus: fake_ecu::answer() behind an ISO-TP socket
 * for physical requests (0x7E0, replies on 0x7E8) and one for
 * functional ones (0x7DF). A functional request's reply goes out on the
 * functional socket, whose long replies take their flow control on
 * 0x7DF, as a Linux ISO-TP tester sending on 0x7DF gives it. */
class Ecu {
public:
    explicit Ecu(long unsupported_nrc = -1, const char* ifname = interface_name())
        : unsupported_nrc_(unsupported_nrc),
          physical_(open_isotp(kReply, kRequest, ifname)),
          functional_(open_isotp(kReply, kFunctional, ifname)),
          stop_(::eventfd(0, EFD_CLOEXEC))
    {
        if (ok()) {
            thread_ = std::thread([this] { run(); });
        }
    }
    Ecu(const Ecu&) = delete;
    Ecu& operator=(const Ecu&) = delete;
    ~Ecu()
    {
        if (thread_.joinable()) {
            const uint64_t one = 1;
            (void)!::write(stop_.get(), &one, sizeof(one));
            thread_.join();
        }
    }
    bool ok() const { return physical_.ok() && functional_.ok() && stop_.ok(); }
    /* Requests answered so far (a refusal counts; silence doesn't) */
    unsigned answered() const { return answered_.load(); }

private:
    void run()
    {
        std::vector<unsigned char> req(4096);
        std::vector<unsigned char> reply(ECUIO_PT_MSG_DATA_SIZE);
        for (;;) {
            pollfd p[3] = {{physical_.get(), POLLIN, 0},
                           {functional_.get(), POLLIN, 0},
                           {stop_.get(), POLLIN, 0}};
            if (::poll(p, 3, -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if (p[2].revents != 0) {
                return;
            }
            for (int i = 0; i < 2; ++i) {
                if ((p[i].revents & POLLIN) == 0) {
                    continue;
                }
                const int fd = p[i].fd;
                const ssize_t n = ::read(fd, req.data(), req.size());
                size_t out_len = 0;
                if (n > 0
                    && fake_ecu::answer(req.data(), static_cast<size_t>(n), unsupported_nrc_,
                                        reply.data(), &out_len)) {
                    if (::write(fd, reply.data(), out_len) == static_cast<ssize_t>(out_len)) {
                        answered_.fetch_add(1);
                    }
                }
            }
        }
    }

    long unsupported_nrc_;
    Fd physical_;
    Fd functional_;
    Fd stop_;
    std::atomic<unsigned> answered_{0};
    std::thread thread_;
};

/* One message of a recording's frames.csv */
struct Message {
    uint32_t id = 0;
    std::vector<uint8_t> data;
    int64_t host_us = 0;
};

/* A recording's messages (ADR-0017's frames.csv: host_us, timestamp_us,
 * id, flags, data as hex bytes), at most max of them. Empty if the file
 * doesn't open; a row that doesn't parse ends the list. */
inline std::vector<Message> read_frames_csv(const char* path, size_t max = SIZE_MAX)
{
    std::vector<Message> out;
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) {
        return out;
    }
    char line[512];
    (void)!std::fgets(line, sizeof(line), f); /* the header */
    while (out.size() < max && std::fgets(line, sizeof(line), f) != nullptr) {
        long long host = 0;
        long long stamp = 0;
        unsigned id = 0;
        unsigned flags = 0;
        int used = 0;
        if (std::sscanf(line, "%lld,%lld,%x,%x,%n", &host, &stamp, &id, &flags, &used) != 4) {
            break;
        }
        Message m;
        m.id = id;
        m.host_us = host;
        for (const char* p = line + used; *p != '\0';) {
            unsigned byte = 0;
            int len = 0;
            if (std::sscanf(p, "%2x%n", &byte, &len) != 1) {
                break;
            }
            m.data.push_back(static_cast<uint8_t>(byte));
            p += len;
            while (*p == ' ') {
                ++p;
            }
        }
        out.push_back(std::move(m));
    }
    std::fclose(f);
    return out;
}

/* Puts each message on the bus as an ECU would: a Single Frame (PCI
 * length, then the data, padded). speed 1 keeps the recording's pace,
 * 10 is ten times faster, 0 as fast as the socket takes them. Returns
 * how many went out; a message longer than a Single Frame holds stops
 * it (none in a J2534 recording of an OBD-II session). */
inline size_t play(const std::vector<Message>& messages, double speed,
                   const char* ifname = interface_name())
{
    Fd s = open_raw(ifname);
    if (!s.ok()) {
        return 0;
    }
    const auto start = std::chrono::steady_clock::now();
    const int64_t first = messages.empty() ? 0 : messages.front().host_us;
    size_t sent = 0;
    for (const Message& m : messages) {
        if (m.data.size() > 7) {
            break;
        }
        if (speed > 0) {
            const auto due = start + std::chrono::microseconds(static_cast<int64_t>(
                                         static_cast<double>(m.host_us - first) / speed));
            std::this_thread::sleep_until(due);
        }
        can_frame f{};
        f.can_id = can_id(m.id);
        f.can_dlc = 8;
        std::memset(f.data, kPad, sizeof(f.data));
        f.data[0] = static_cast<uint8_t>(m.data.size());
        std::memcpy(f.data + 1, m.data.data(), m.data.size());
        /* vcan has no flow control: a full send queue says ENOBUFS */
        while (::write(s.get(), &f, sizeof(f)) != static_cast<ssize_t>(sizeof(f))) {
            if (errno != ENOBUFS) {
                return sent;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        ++sent;
    }
    return sent;
}

} // namespace vcan_bus

#endif /* ECUIO_TESTS_VCAN_BUS_H */
