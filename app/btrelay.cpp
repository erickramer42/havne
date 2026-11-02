// File/layer: app/btrelay.cpp | Qt6 Core/Bluetooth, app layer, C++17;
//             built on Android only (APP_BLUETOOTH)
// Testing: CI's android-build compiles it; BENCH and Vehicle on the phone
//          (ADR-0047); its rules and the socket's retirement are
//          UNIT-tested (btconnect.h, btretire.h)

#include "btrelay.h"
#include "btretire.h"

#include <QBluetoothSocket>
#include <QBluetoothUuid>

BtRelayAdapter::BtRelayAdapter(const QString& address, QObject* parent)
    : QObject(parent), m_address(address)
{}

BtRelayAdapter::~BtRelayAdapter()
{
    dropSocket();
}

void BtRelayAdapter::open()
{
    act(m_rules.open());
}

void BtRelayAdapter::write(const QByteArray& bytes)
{
    if (!m_rules.up() || m_socket == nullptr) {
        return;
    }
    // A failed write emits errorOccurred before it returns, which drops
    // the socket (btstream.cpp's write, the same)
    if (m_socket->write(bytes) != bytes.size() && m_socket != nullptr) {
        act(m_rules.lost(),
            tr("the write failed: %1").arg(m_socket->errorString()));
    }
}

void BtRelayAdapter::close()
{
    act(m_rules.close());
}

void BtRelayAdapter::act(BtConnectRules::Action action, const QString& why)
{
    using Action = BtConnectRules::Action;
    switch (action) {
    case Action::None:
        break;
    case Action::Connect:
        dropSocket();
        connectSocket();
        break;
    case Action::ReportOpened:
        if (opened) {
            opened();
        }
        break;
    case Action::ReportFailed:
        dropSocket();
        if (failed) {
            failed(why);
        }
        break;
    case Action::CloseSocket:
        dropSocket();
        break;
    }
}

void BtRelayAdapter::socketFailed(const QString& why)
{
    if (m_rules.connecting()) {
        const BtConnectRules::Action action = m_rules.connectFailed();
        dropSocket(); // its connect has ended
        act(action, why);
    } else {
        act(m_rules.lost(), why);
    }
}

// Retired, not deleted: Qt's reader thread may still write into it
// (btretire.h)
void BtRelayAdapter::dropSocket()
{
    if (m_socket != nullptr) {
        m_socket->disconnect(this);
        m_socket->abort();
        retireBtSocket(m_socket);
        m_socket = nullptr;
    }
}

void BtRelayAdapter::connectSocket()
{
    m_socket =
        new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol, this);
    connect(m_socket, &QBluetoothSocket::connected, this,
            [this] { act(m_rules.connected()); });
    connect(m_socket, &QBluetoothSocket::readyRead, this, [this] {
        const QByteArray bytes = m_socket->readAll();
        if (m_rules.up() && !bytes.isEmpty() && received) {
            received(bytes);
        }
    });
    connect(m_socket, &QBluetoothSocket::errorOccurred, this,
            [this](QBluetoothSocket::SocketError) {
                socketFailed(m_socket->errorString());
            });
    connect(m_socket, &QBluetoothSocket::disconnected, this,
            [this] { socketFailed(tr("the adapter disconnected")); });
    m_socket->connectToService(
        m_address,
        QBluetoothUuid(QBluetoothUuid::ServiceClassUuid::SerialPort));
}
