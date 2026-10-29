// File/layer: app/bluetoothdevices.h | Qt6 Core/Qml, app layer, C++17
// Testing: test_qml (tests/qml/tst_settings.qml): the Bluetooth rows in
//          Settings' device list and the field they fill; a bt: URI on the
//          desktop refused before core is asked, its reason in the error
//          line. BENCH — the phone (ADR-0028 §4): the permission prompt,
//          the paired list, the BLE scan; Vehicle — the MX+ through it

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <map>

class QJSEngine;
class QQmlEngine;
class SessionBridge;
#ifdef APP_BLUETOOTH
class BtLink;
class QBluetoothDeviceDiscoveryAgent;
#endif

// The phone's Bluetooth adapters for Settings, and the links core reads
// them through (ADR-0028 §2). The paired adapters come from Android's own
// list, a BLE adapter from a scan Settings starts; each row is
// elm327://bt:<address> (classic, the SPP service) or elm327://ble:<address>.
// Before a connect the bridge asks prepare(), which registers that
// address's app-fed stream with core (btstream.h) the first time.
// Pairing stays in Android's settings.
//
// Built with QtBluetooth on Android only (APP_BLUETOOTH); elsewhere
// `available` is false, the list empty, and a bt: or ble: URI is refused
// with what to use instead. Exposed to QML as the `Bluetooth` singleton.
class BluetoothDevices : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Bluetooth)
    QML_SINGLETON
    // This build reaches Bluetooth adapters
    Q_PROPERTY(bool available READ available CONSTANT)
    // [{ name, address, kind }], kind "bt" or "ble": the paired adapters,
    // then those the last scan found that aren't paired
    Q_PROPERTY(QVariantList adapters READ adapters NOTIFY adaptersChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    // Why the list is short, in a sentence ("Bluetooth is off"), or ""
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // The status asks the user to act (Bluetooth off, the permission
    // refused, a failed scan), not just reports
    Q_PROPERTY(bool statusWarns READ statusWarns NOTIFY statusChanged)

public:
    explicit BluetoothDevices(QObject* parent = nullptr);
    ~BluetoothDevices() override;

    // main()'s, set before the engine loads QML; without one (the QML
    // tests' own) the engine makes and owns one
    inline static BluetoothDevices* instance = nullptr;
    static BluetoothDevices* create(QQmlEngine*, QJSEngine* engine);

    // Hands the bridge prepare() for every connect. Destroy this after the
    // bridge's sessions are closed: the streams unregister here.
    void attachTo(SessionBridge* bridge);

    bool available() const;
    QVariantList adapters() const;
    bool scanning() const { return m_scanning; }
    QString status() const { return m_status; }
    bool statusWarns() const { return m_statusWarns; }

    // Lists the paired adapters. With ask, asks for the permission when
    // Android hasn't been asked yet; without, lists only once it's given
    Q_INVOKABLE void refresh(bool ask);
    // Looks for BLE adapters for a few seconds; they join the list
    Q_INVOKABLE void scan();

    // The stream an elm327:// URI names, "bt:<address>" or
    // "ble:<address>", or "" for any other URI
    static QString streamName(const QString& uri);
    // GUI thread, before a connect: "" when uri names no stream or its
    // stream is registered, otherwise why it can't be
    QString prepare(const QString& uri);

signals:
    void adaptersChanged();
    void scanningChanged();
    void statusChanged();

private:
    void setStatus(const QString& status, bool warns = false);
    void setScanning(bool scanning);

    QVariantList m_paired;
    QVariantList m_scanned;
    bool m_scanning = false;
    QString m_status;
    bool m_statusWarns = false;
#ifdef APP_BLUETOOTH
    // Whether Android has been asked and what it said
    bool permitted(bool ask, bool forScan);
    void listPaired();
    void scanNow();
    std::map<QString, BtLink*> m_links; // by stream name, owned (children)
    QBluetoothDeviceDiscoveryAgent* m_agent = nullptr;
#endif
};
