// File/layer: app/btconnect.h | C++17, app layer, no Qt
// Testing: UNIT: BridgeTest.BtConnect* (test_bridge): an open while a
//          connect runs adopts it, an abandoned connect that succeeds is
//          closed, a failed adopted connect starts a fresh one, one connect
//          at a time throughout; BENCH: the phone (ADR-0029 §5)

#pragma once

// When a classic Bluetooth link connects, as plain rules (ADR-0029 §3), so
// they are testable without a phone. QtBluetooth's Android socket connects
// through Java's blocking connect(), which an abort can't cut short: the
// connect runs to its end, up to half a minute, whatever the link does. So
// a connect is never abandoned by starting another. A close while it runs
// leaves it running; an open while it runs waits for it and adopts it;
// one that succeeds with no open waiting is closed.
//
// The link (btstream.cpp) feeds in core's calls and the socket's results,
// and does what each returns.
class BtConnectRules
{
public:
    enum class Action {
        None,
        Connect,      // start a socket connect
        ReportOpened, // tell core the link is open
        ReportFailed, // tell core the open or the link failed
        CloseSocket,  // close the socket: nothing wants it
    };

    // Core asks for the link
    Action open();
    // Core is done with it, or gives up on an open
    Action close();
    // The socket connected
    Action connected();
    // The socket's connect failed
    Action connectFailed();
    // The open link dropped
    Action lost();

    bool up() const { return m_up; }
    bool connecting() const { return m_connecting; }
    bool wanted() const { return m_wanted; }

private:
    bool m_wanted = false;     // an open is waiting for the link
    bool m_connecting = false; // a socket connect is running
    bool m_adopted = false;    // that connect began for an earlier open
    bool m_up = false;         // the link is open and core has been told
};
