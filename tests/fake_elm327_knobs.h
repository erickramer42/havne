// File/layer: tests/fake_elm327_knobs.h | test-only, C++17, Qt-free, no
//             platform headers; header-only
// Testing: HARNESS: Elm327SessionTest, Elm327PortTest (test_elm327)
//
// The fake ELM327's knobs (tests/fake_elm327.h): what a test sets to
// make the adapter or its ECU behave as a real one was seen to.
//
// Knobs (fake_elm327::knobs), set under the port's lock (with()):
//   plain        a plain ELM327: STI, STVR and every ST command answer '?'
//   version      what AT Z and AT I answer ("ELM327 v1.4b")
//   reset_ok     AT Z and AT WS answer OK, and only AT I the version, as
//                the OBDX Pro VX does
//   refuse       one command, without its CR ("ATH1"), answered '?'
//   no_data      every request answered NO DATA
//   can_error    every request answered CAN ERROR (no node to ACK)
//   silent       requests get nothing at all, not even the prompt
//   pending      each request's reply follows 7F <service> 78 in the same
//                read, as an STN prints and listens past it;
//                pending_count of them, pending_gap_ms apart (an ECU
//                repeats it while it works)
//   pending_counted  a response count counts each 7F xx 78 as a line,
//                so a request with the digit 1 ends after the first
//                one, and the answer never prints (an adapter that
//                doesn't wait past a pending reply)
//   reply_delay_ms  each request's reply and prompt this long after it
//   input_stops  a byte from the host while a request's reply is still
//                due stops it, as the datasheet's adapter does: what's
//                left of the reply is dropped, the byte and the rest of
//                its write discarded (an assumption: the MX+ never ran
//                the command typed into its reply,
//                docs/bench-sessions/2026-11-11-ext-session-car.md), and
//                STOPPED and the prompt follow; counted in interrupts
//   stopped_once one command, without its CR ("1001"), answered STOPPED
//                once and not run
//   unsupported_nrc fake_ecu::answer's (-1 the ECU's codes, 0 silence)
//   four_digit_ids  11-bit IDs printed as two bytes (07E8)
//   pp2a, pp2a_on   AT PPS's parameter 2A
//   dropped      the link is down: the port's reads and writes fail, and
//                opening it says "no such port"
//   hung         while monitoring, a stop gets nothing back and the
//                monitor prints nothing more: no STOPPED, no prompt
//   full_ends_monitor  BUFFER FULL ends the monitor with a prompt, as a
//                plain ELM327's does
//   stale_banner the next link opens with a reset's banner and prompt
//                already on it, which arrive ahead of the first
//                command's answer, and that link's first reset answers
//                kLateResetMs late, after anything sent meanwhile was
//                taken (the MX+ over a new Bluetooth link,
//                docs/bench-sessions/2026-10-29-android.md); one link
//   cut_gap_ms   a reply the count digit cut goes on on the bus, one
//                Consecutive Frame this often, and the ECU answers no
//                request until it's sent (NO DATA): an ECU the adapter
//                sent flow control (the test car's engine ECU, 8 ms;
//                docs/bench-sessions/2026-11-01-android.md); 0 none
//   monitor_eats_cfs  while monitoring, a message's First Frame prints
//                and its Consecutive Frames don't, under AT CFC1 or
//                CFC0, as the MX+'s STN2255 does
//                (docs/bench-sessions/2026-11-04-two-ecu-car.md,
//                docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md)
//   longer_pid   a Service 01 PID (-1 none) whose positive reply the ECU
//                pads with zeros to 13 data bytes, a First Frame and two
//                Consecutive Frames: an ECU answering longer than J1979's
//                length for it

#ifndef ECUIO_TESTS_FAKE_ELM327_KNOBS_H
#define ECUIO_TESTS_FAKE_ELM327_KNOBS_H

#include <cstdint>

namespace fake_elm327 {

struct knobs {
    bool plain = false;
    const char* version = "ELM327 v1.4b";
    bool reset_ok = false;
    const char* refuse = nullptr;
    bool no_data = false;
    bool can_error = false;
    bool silent = false;
    bool pending = false;
    uint32_t pending_count = 1;
    uint32_t pending_gap_ms = 0;
    bool pending_counted = false;
    uint32_t reply_delay_ms = 0;
    bool input_stops = false;
    const char* stopped_once = nullptr;
    long unsupported_nrc = -1;
    bool four_digit_ids = false;
    uint8_t pp2a = 0x04;
    bool pp2a_on = false;
    bool dropped = false;
    bool hung = false;
    bool full_ends_monitor = false;
    bool stale_banner = false;
    uint32_t cut_gap_ms = 0;
    bool monitor_eats_cfs = false;
    int longer_pid = -1;
};

} // namespace fake_elm327

#endif /* ECUIO_TESTS_FAKE_ELM327_KNOBS_H */
