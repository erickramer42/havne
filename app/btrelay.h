// File/layer: app/btrelay.h | Qt6 Core/Bluetooth, app layer, C++17;
//             built on Android only (APP_BLUETOOTH)
// Testing: CI android-build (compiles it); BENCH, Vehicle (the test phone)

#pragma once

#include <QBluetoothAddress>
#include <QObject>
#include <QString>

#include "adapterrelay.h"
#include "btconnect.h"

class QBluetoothSocket;

// A paired classic adapter's link for a relay (ADR-0047): an RFCOMM
// socket to its SPP service, connected by btconnect.h's rules as
// btstream.cpp's link is (one connect at a time) and retired as it is
// (btretire.h), its bytes handed to the relay rather than to core.
class BtRelayAdapter : public QObject, public RelayAdapter
{
    Q_OBJECT

public:
    explicit BtRelayAdapter(const QString& address, QObject* parent = nullptr);
    ~BtRelayAdapter() override;

    void open() override;
    void write(const QByteArray& bytes) override;
    void close() override;

private:
    void act(BtConnectRules::Action action, const QString& why = {});
    void socketFailed(const QString& why);
    void dropSocket();
    void connectSocket();

    QBluetoothAddress m_address;
    BtConnectRules m_rules;
    QBluetoothSocket* m_socket = nullptr;
};
