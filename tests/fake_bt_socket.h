// File/layer: tests/fake_bt_socket.h | test-only, Qt6 Core, C++17
// Testing: the stand-in for QBluetoothSocket in BridgeTest.BtRetire*
//          (test_bridge), where Qt's Bluetooth module isn't built
//
// The shape app/btretire.h needs of a socket: a state, set by the test
// as Qt's Android socket sets it (UnconnectedState once its Java reader
// has stopped), and the signal that says so.

#pragma once

#include <QObject>

class FakeBtSocket : public QObject
{
    Q_OBJECT

public:
    enum class SocketState { UnconnectedState, ConnectedState, ClosingState };

    explicit FakeBtSocket(SocketState state, QObject* parent = nullptr)
        : QObject(parent), m_state(state)
    {}

    SocketState state() const { return m_state; }
    void setState(SocketState state)
    {
        m_state = state;
        emit stateChanged(state);
    }

signals:
    void stateChanged(FakeBtSocket::SocketState state);

private:
    SocketState m_state;
};
