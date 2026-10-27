// File/layer: tests/test_serial_ports.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT — serial-port discovery (core/include/ecuio/serial.h,
//          core/src/serial_ports.h; ADR-0026 §7): the order with digit
//          runs by value, the /dev names listed on macOS and elsewhere,
//          the dedupe and the copy up to a capacity over made-up ports,
//          the null arguments, and the contract against the machine's
//          own ports, which most CI runners lack. Qt-free.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ecuio/serial.h"
#include "elm327_port.h"  /* core/src: the names elm327:// takes */
#include "serial_ports.h" /* core/src */

namespace {

ecuio_serial_port_t port(const char* name, const char* device)
{
    ecuio_serial_port_t p;
    EXPECT_TRUE(serial_port_make(name, device, &p)) << name;
    return p;
}

/* A port nothing wrote: every byte 0x5A */
ecuio_serial_port_t untouched()
{
    ecuio_serial_port_t p;
    memset(&p, 0x5A, sizeof(p));
    return p;
}

bool is_untouched(const ecuio_serial_port_t& p)
{
    const ecuio_serial_port_t u = untouched();
    return memcmp(&p, &u, sizeof(p)) == 0;
}

TEST(SerialPortsTest, NamesSortWithTheirNumbersByValue)
{
    EXPECT_TRUE(serial_name_less("COM2", "COM10"));
    EXPECT_FALSE(serial_name_less("COM10", "COM2"));
    EXPECT_TRUE(serial_name_less("COM1", "COM2"));
    EXPECT_TRUE(serial_name_less("/dev/ttyUSB9", "/dev/ttyUSB10"));
    EXPECT_TRUE(serial_name_less("/dev/ttyACM3", "/dev/ttyUSB0"));
    EXPECT_TRUE(
        serial_name_less("/dev/cu.usbserial-2", "/dev/cu.usbserial-10"));
    EXPECT_TRUE(serial_name_less("COM5", "COM5a")) << "a prefix first";
    EXPECT_FALSE(serial_name_less("COM5", "COM5"));
    /* Equal by value: neither first, so the list breaks the tie */
    EXPECT_FALSE(serial_name_less("COM5", "COM05"));
    EXPECT_FALSE(serial_name_less("COM05", "COM5"));
}

TEST(SerialPortsTest, DevNamesAreListedByEachSystemsPrefixes)
{
    for (const char* listed : {"ttyUSB0", "ttyUSB12", "ttyACM0", "rfcomm0"}) {
        EXPECT_TRUE(serial_dev_listed(listed, false)) << listed;
        EXPECT_FALSE(serial_dev_listed(listed, true)) << listed << " on macOS";
    }
    for (const char* other : {"ttyUSB", "ttyUSB0a", "ttyS0", "tty0", "ttyACM",
                              "rfcomm", "cu.usbserial-1420", "null"}) {
        EXPECT_FALSE(serial_dev_listed(other, false)) << other;
    }
    for (const char* listed : {"cu.usbserial-1420", "cu.OBDLinkMXplus-SPPDev",
                               "cu.Bluetooth-Incoming-Port"}) {
        EXPECT_TRUE(serial_dev_listed(listed, true)) << listed;
    }
    for (const char* other : {"cu.", "tty.usbserial-1420", "cua0", "null"}) {
        EXPECT_FALSE(serial_dev_listed(other, true)) << other;
    }
}

TEST(SerialPortsTest, ANameThatDoesntFitIsLeftOutAndALongDeviceLeftEmpty)
{
    ecuio_serial_port_t p;
    EXPECT_FALSE(serial_port_make("", "", &p));
    const std::string longest(ECUIO_SERIAL_NAME_LEN - 1, 'x');
    ASSERT_TRUE(serial_port_make(longest.c_str(), "", &p));
    EXPECT_EQ(std::string(p.name), longest);
    const std::string too_long(ECUIO_SERIAL_NAME_LEN, 'x');
    EXPECT_FALSE(serial_port_make(too_long.c_str(), "", &p));

    const std::string device(ECUIO_SERIAL_DEVICE_LEN, 'd');
    ASSERT_TRUE(serial_port_make("COM5", device.c_str(), &p));
    EXPECT_STREQ(p.device, "");
    EXPECT_EQ(p.flags, 0u);
}

/* Sorted, each name once (the first kept), *count all of them and only
 * the capacity's worth copied */
TEST(SerialPortsTest, TheListIsSortedWithoutRepeatsAndCopiedUpToTheCapacity)
{
    const std::vector<ecuio_serial_port_t> candidates = {
        port("COM10", "\\Device\\VCP1"),
        port("COM2", "\\Device\\BthModem0"),
        port("COM1", "\\Device\\Serial0"),
        port("COM2", "\\Device\\BthModem1"),
        port("COM05", "\\Device\\USBSER000"),
        port("COM5", "\\Device\\USBSER001"),
        port("COM05", "\\Device\\USBSER002"),
    };

    std::vector<ecuio_serial_port_t> found = candidates;
    std::vector<ecuio_serial_port_t> out(8, untouched());
    uint32_t count = 99;
    serial_ports_finish(found, out.data(), static_cast<uint32_t>(out.size()),
                        &count);
    ASSERT_EQ(count, 5u);
    const char* names[] = {"COM1", "COM2", "COM05", "COM5", "COM10"};
    for (uint32_t i = 0; i < count; ++i) {
        EXPECT_STREQ(out[i].name, names[i]) << i;
    }
    EXPECT_STREQ(out[1].device, "\\Device\\BthModem0") << "the first COM2 kept";
    EXPECT_STREQ(out[2].device, "\\Device\\USBSER000")
        << "the first COM05 kept";
    EXPECT_TRUE(is_untouched(out[5]));

    found = candidates;
    out.assign(8, untouched());
    count = 99;
    serial_ports_finish(found, out.data(), 2, &count);
    EXPECT_EQ(count, 5u) << "the count is all of them";
    EXPECT_STREQ(out[0].name, "COM1");
    EXPECT_STREQ(out[1].name, "COM2");
    EXPECT_TRUE(is_untouched(out[2])) << "nothing past the capacity";

    found = candidates;
    count = 99;
    serial_ports_finish(found, nullptr, 0, &count);
    EXPECT_EQ(count, 5u) << "capacity 0 asks for the count";

    found.clear();
    count = 99;
    serial_ports_finish(found, out.data(), 8, &count);
    EXPECT_EQ(count, 0u);
}

TEST(SerialPortsTest, NullArgumentsAreRefused)
{
    ecuio_serial_port_t p;
    uint32_t count = 99;
    EXPECT_EQ(ecuio_serial_ports(&p, 1, nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_serial_ports(nullptr, 1, &count), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_serial_ports(nullptr, 0, &count), ECUIO_OK);
}

/* The machine's own ports, whatever they are (most CI runners have
 * none): each a name elm327:// takes, terminated, in order, once, with
 * nothing written past the count */
TEST(SerialPortsTest, TheMachinesPortsKeepTheContract)
{
    uint32_t n = 99;
    ASSERT_EQ(ecuio_serial_ports(nullptr, 0, &n), ECUIO_OK);
    std::vector<ecuio_serial_port_t> ports(n + 1, untouched());
    uint32_t count = 99;
    ASSERT_EQ(ecuio_serial_ports(ports.data(), n + 1, &count), ECUIO_OK);
    ASSERT_EQ(count, n) << "a port came or went between the two calls";
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_serial_port_t& p = ports[i];
        ASSERT_NE(memchr(p.name, '\0', sizeof(p.name)), nullptr) << i;
        ASSERT_NE(memchr(p.device, '\0', sizeof(p.device)), nullptr) << i;
        EXPECT_TRUE(elm327_port_name_ok(p.name, strlen(p.name))) << p.name;
#ifndef _WIN32
        EXPECT_STREQ(p.device, "") << p.name;
#endif
        EXPECT_EQ(p.flags, 0u) << p.name;
        for (const uint32_t r : p.reserved) {
            EXPECT_EQ(r, 0u) << p.name;
        }
        if (i > 0) {
            EXPECT_FALSE(serial_name_less(p.name, ports[i - 1].name)) << p.name;
            EXPECT_STRNE(p.name, ports[i - 1].name);
        }
    }
    EXPECT_TRUE(is_untouched(ports[count]));
}

} // namespace
