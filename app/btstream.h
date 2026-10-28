// File/layer: app/btstream.h | Qt6 Core/Bluetooth, app layer, C++17;
//             built on Android only (QTTUNE_BLUETOOTH)
// Testing: CI's android-build compiles it; the BLE service choice is
//          blepicker.h's, UNIT-tested; BENCH — the phone (ADR-0028 §4);
//          Vehicle — the MX+ through Android's classic Bluetooth. BLE is
//          fake-tested in core only, until a BLE adapter is on hand

#pragma once

#include <QByteArray>
#include <QBluetoothAddress>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyService>
#include <QObject>
#include <QString>

#include <vector>

#include "ecuio/stream.h"

class QBluetoothSocket;
class QLowEnergyController;

// One adapter's link behind an app-fed stream (ADR-0028, ecuio/stream.h):
// core calls open, write and close on its own threads; each is queued to
// this object's thread (the GUI thread, where QtBluetooth delivers its
// signals) and returns at once, and the link's answers go back as
// ecuio_stream_opened, _received and _failed. Core's backoff paces the
// reopens, so the link never retries on its own.
//   Classic: an RFCOMM socket to the SPP service of a paired adapter.
//   BLE: a GATT connection, its services discovered and the one
//        pickBleService chooses, notifications on, writes split to the
//        ATT MTU less 3.
class BtLink : public QObject
{
    Q_OBJECT

public:
    enum class Kind { Classic, LowEnergy };

    BtLink(Kind kind, const QString& address, QObject* parent = nullptr);
    // Unregisters the stream; its session is closed by then
    ~BtLink() override;

    // Registers the stream as name: "" or why core refused it
    QString registerAs(const QString& name);

private:
    // Core's callbacks, on core's threads
    static void onOpen(void* user);
    static void onWrite(void* user, const uint8_t* data, uint32_t len);
    static void onClose(void* user);

    // This object's thread
    void open();
    void write(const QByteArray& bytes);
    void close();
    void opened();
    void fail(const QString& reason);
    void teardown();

    void openClassic();
    void openLowEnergy();
    void servicesFound();
    void serviceDiscovered();
    void subscribe();

    Kind m_kind;
    QBluetoothAddress m_address;
    ecuio_stream_t* m_stream = nullptr;
    // Between open and close: opening until the link is up, then up
    bool m_opening = false;
    bool m_up = false;

    QBluetoothSocket* m_socket = nullptr;
    QLowEnergyController* m_controller = nullptr;
    std::vector<QLowEnergyService*> m_services; // the controller's children
    int m_undiscovered = 0;
    QLowEnergyService* m_service = nullptr;
    QLowEnergyCharacteristic m_writeTo;
    QLowEnergyCharacteristic m_notifyFrom;
    QLowEnergyService::WriteMode m_writeMode =
        QLowEnergyService::WriteWithResponse;
};
