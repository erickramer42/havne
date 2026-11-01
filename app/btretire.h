// File/layer: app/btretire.h | Qt6 Core, app layer, C++17, header-only;
//             every platform (the link that uses it, btstream.cpp, is
//             Android's)
// Testing: UNIT: BridgeTest.BtRetire* (test_bridge, against
//          tests/fake_bt_socket.h): deleted at once when it's already
//          unconnected, kept past its old parent and every other state
//          until it says unconnected, deleted by the fallback when it
//          never does; Vehicle: disconnects while replies arrive, on the
//          phone with the MX+
//
// An aborted classic Bluetooth socket outlives its link until it says
// it's unconnected. On Android, QBluetoothSocket's abort() only asks
// Java to close the socket: Qt's Java reader thread goes on reading,
// and each block it reads is copied into the socket's own buffer
// through a pointer to it, until its read() throws. Only then does Qt
// set UnconnectedState (QBluetoothSocketPrivateAndroid::inputThreadError,
// queued from that thread's last call). A socket deleted before that,
// with replies still arriving, is written into after it's freed: the
// SIGSEGV in the QtBtInputStream thread on a Disconnect. So the socket
// leaves its parent (a link going away can't take it along) and goes
// once it's unconnected, or after a fallback for a reader that never
// throws. Where abort() leaves it unconnected at once (an abort
// mid-connect, other platforms), it goes at once.

#pragma once

#include <QObject>
#include <QTimer>

// Longer than Java's read() takes to throw after close(); a fallback
// only, the state is what ends it
constexpr int kBtSocketRetireMs = 5000;

// s: aborted, its signals to the link already cut. Socket is a QObject
// with state(), a SocketState enum holding UnconnectedState, and a
// stateChanged(SocketState) signal (QBluetoothSocket's shape).
template <typename Socket>
void retireBtSocket(Socket* s, int fallbackMs = kBtSocketRetireMs)
{
    using State = typename Socket::SocketState;
    s->setParent(nullptr);
    if (s->state() == State::UnconnectedState) {
        s->deleteLater();
        return;
    }
    QObject::connect(s, &Socket::stateChanged, s, [s](State state) {
        if (state == State::UnconnectedState) {
            s->deleteLater();
        }
    });
    QTimer::singleShot(fallbackMs, s, [s] { s->deleteLater(); });
}
