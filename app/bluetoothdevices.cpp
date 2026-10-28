// File/layer: app/bluetoothdevices.cpp | Qt6 Core/Qml (+ Bluetooth on
//             Android), app layer, C++17
// Testing: test_qml (tests/qml/tst_settings.qml): the rows, and the
//          desktop's refusal of a bt: URI through the bridge; CI's
//          android-build compiles the Android half; BENCH — the phone run
//          (ADR-0028 §4)

#include "bluetoothdevices.h"
#include "qttunebridge.h"

#include <QJSEngine>
#include <QQmlEngine>
#include <QVariantMap>

#include <algorithm>

#ifdef QTTUNE_BLUETOOTH
#include "btstream.h"

#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothLocalDevice>
#include <QCoreApplication>
#include <QPermissions>
#endif

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace {

#ifdef QTTUNE_BLUETOOTH
// How long a scan looks for BLE adapters
constexpr int kScanMs = 8000;
#endif

#ifdef Q_OS_ANDROID
// Android's paired devices (BluetoothAdapter.getBondedDevices), which Qt
// doesn't list: { name, address, kind }, a device of type LE "ble", any
// other "bt". Needs BLUETOOTH_CONNECT; empty without the adapter.
QVariantList bondedDevices()
{
    QVariantList out;
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject service =
        QJniObject::fromString(QStringLiteral("bluetooth"));
    const QJniObject manager = context.callObjectMethod(
        "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
        service.object<jstring>());
    if (!manager.isValid()) {
        return out;
    }
    const QJniObject adapter = manager.callObjectMethod(
        "getAdapter", "()Landroid/bluetooth/BluetoothAdapter;");
    if (!adapter.isValid()) {
        return out;
    }
    const QJniObject bonded =
        adapter.callObjectMethod("getBondedDevices", "()Ljava/util/Set;");
    if (!bonded.isValid()) {
        return out;
    }
    const QJniObject array =
        bonded.callObjectMethod("toArray", "()[Ljava/lang/Object;");
    if (!array.isValid()) {
        return out;
    }
    QJniEnvironment env;
    const auto items = static_cast<jobjectArray>(array.object());
    const jsize n = env->GetArrayLength(items);
    for (jsize i = 0; i < n; ++i) {
        const QJniObject device =
            QJniObject::fromLocalRef(env->GetObjectArrayElement(items, i));
        if (!device.isValid()) {
            continue;
        }
        const QString address =
            device.callObjectMethod("getAddress", "()Ljava/lang/String;")
                .toString();
        const QString name =
            device.callObjectMethod("getName", "()Ljava/lang/String;")
                .toString();
        const jint type = device.callMethod<jint>("getType", "()I");
        constexpr jint kDeviceTypeLe = 2; // BluetoothDevice.DEVICE_TYPE_LE
        out.append(QVariantMap{{QStringLiteral("name"), name},
                               {QStringLiteral("address"), address},
                               {QStringLiteral("kind"),
                                type == kDeviceTypeLe ? QStringLiteral("ble")
                                                      : QStringLiteral("bt")}});
    }
    env.checkAndClearExceptions();
    std::sort(out.begin(), out.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap()
                   .value(QStringLiteral("name"))
                   .toString()
                   .compare(b.toMap().value(QStringLiteral("name")).toString(),
                            Qt::CaseInsensitive) < 0;
    });
    return out;
}
#endif

} // namespace

BluetoothDevices::BluetoothDevices(QObject* parent) : QObject(parent) {}

// The links' streams unregister with them (BtLink's destructor); the
// bridge has closed its sessions by now (attachTo)
BluetoothDevices::~BluetoothDevices() = default;

BluetoothDevices* BluetoothDevices::create(QQmlEngine*, QJSEngine* engine)
{
    if (instance != nullptr) {
        Q_ASSERT(engine->thread() == instance->thread());
        // main() owns it; the engine must never delete it
        QJSEngine::setObjectOwnership(instance, QJSEngine::CppOwnership);
        return instance;
    }
    return new BluetoothDevices; // the engine's
}

void BluetoothDevices::attachTo(QtTuneBridge* bridge)
{
    bridge->setStreamPreparer(
        [this](const QString& uri) { return prepare(uri); });
}

bool BluetoothDevices::available() const
{
#ifdef QTTUNE_BLUETOOTH
    return true;
#else
    return false;
#endif
}

QVariantList BluetoothDevices::adapters() const
{
    QVariantList all = m_paired;
    for (const QVariant& v : m_scanned) {
        const QString address =
            v.toMap().value(QStringLiteral("address")).toString();
        const bool paired = std::any_of(
            m_paired.cbegin(), m_paired.cend(), [&](const QVariant& p) {
                return p.toMap().value(QStringLiteral("address")).toString() ==
                       address;
            });
        if (!paired) {
            all.append(v);
        }
    }
    return all;
}

QString BluetoothDevices::streamName(const QString& uri)
{
    static const QString scheme = QStringLiteral("elm327://");
    if (!uri.startsWith(scheme)) {
        return {};
    }
    QString port = uri.mid(scheme.size());
    const qsizetype q = port.indexOf(QLatin1Char('?'));
    if (q >= 0) {
        port.truncate(q);
    }
    if (port.startsWith(QLatin1String("bt:")) ||
        port.startsWith(QLatin1String("ble:"))) {
        return port;
    }
    return {};
}

void BluetoothDevices::setStatus(const QString& status)
{
    if (m_status != status) {
        m_status = status;
        emit statusChanged();
    }
}

void BluetoothDevices::setScanning(bool scanning)
{
    if (m_scanning != scanning) {
        m_scanning = scanning;
        emit scanningChanged();
    }
}

#ifndef QTTUNE_BLUETOOTH

void BluetoothDevices::refresh(bool) {}
void BluetoothDevices::scan() {}

QString BluetoothDevices::prepare(const QString& uri)
{
    const QString name = streamName(uri);
    if (name.isEmpty()) {
        return {};
    }
    return tr("%1: Bluetooth links open in the Android app; on a computer, "
              "pair the adapter and connect to its serial port")
        .arg(name);
}

#else

// Android's answer on the Nearby devices permission (BLUETOOTH_CONNECT and
// BLUETOOTH_SCAN, declared never for location: app/CMakeLists.txt). With
// ask, an undecided one is asked, and the list or the scan runs on a yes.
bool BluetoothDevices::permitted(bool ask, bool forScan)
{
    QBluetoothPermission permission;
    permission.setCommunicationModes(QBluetoothPermission::Access);
    switch (qApp->checkPermission(permission)) {
    case Qt::PermissionStatus::Granted:
        return true;
    case Qt::PermissionStatus::Denied:
        setStatus(tr("Allow Nearby devices for QtTune in Android's settings to "
                     "reach Bluetooth adapters"));
        return false;
    case Qt::PermissionStatus::Undetermined:
        break;
    }
    if (!ask) {
        return false;
    }
    qApp->requestPermission(
        permission, this, [this, forScan](const QPermission& answer) {
            if (answer.status() == Qt::PermissionStatus::Granted) {
                if (forScan) {
                    scanNow();
                } else {
                    listPaired();
                }
            } else {
                setStatus(
                    tr("Allow Nearby devices for QtTune in Android's settings "
                       "to reach Bluetooth adapters"));
            }
        });
    return false;
}

void BluetoothDevices::refresh(bool ask)
{
    if (permitted(ask, false)) {
        listPaired();
    }
}

void BluetoothDevices::listPaired()
{
    const QBluetoothLocalDevice local;
    if (!local.isValid()) {
        setStatus(tr("This device has no Bluetooth"));
        return;
    }
    if (local.hostMode() == QBluetoothLocalDevice::HostPoweredOff) {
        setStatus(tr("Bluetooth is off: turn it on, then Refresh"));
        if (!m_paired.isEmpty()) {
            m_paired.clear();
            emit adaptersChanged();
        }
        return;
    }
    setStatus({});
#ifdef Q_OS_ANDROID
    m_paired = bondedDevices();
#endif
    emit adaptersChanged();
}

void BluetoothDevices::scan()
{
    if (!m_scanning && permitted(true, true)) {
        scanNow();
    }
}

void BluetoothDevices::scanNow()
{
    if (m_agent == nullptr) {
        m_agent = new QBluetoothDeviceDiscoveryAgent(this);
        m_agent->setLowEnergyDiscoveryTimeout(kScanMs);
        connect(
            m_agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this,
            [this](const QBluetoothDeviceInfo& info) {
                if (!(info.coreConfigurations() &
                      QBluetoothDeviceInfo::LowEnergyCoreConfiguration)) {
                    return;
                }
                const QString address = info.address().toString();
                for (const QVariant& v : std::as_const(m_scanned)) {
                    if (v.toMap().value(QStringLiteral("address")).toString() ==
                        address) {
                        return;
                    }
                }
                m_scanned.append(QVariantMap{
                    {QStringLiteral("name"), info.name()},
                    {QStringLiteral("address"), address},
                    {QStringLiteral("kind"), QStringLiteral("ble")}});
                emit adaptersChanged();
            });
        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::finished, this,
                [this] {
                    setScanning(false);
                    setStatus(m_scanned.isEmpty()
                                  ? tr("The scan found no BLE adapters")
                                  : QString());
                });
        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::canceled, this,
                [this] { setScanning(false); });
        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this,
                [this](QBluetoothDeviceDiscoveryAgent::Error) {
                    setScanning(false);
                    setStatus(
                        tr("The scan failed: %1").arg(m_agent->errorString()));
                });
    }
    m_scanned.clear();
    emit adaptersChanged();
    setStatus(tr("Looking for BLE adapters…"));
    setScanning(true);
    m_agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

QString BluetoothDevices::prepare(const QString& uri)
{
    const QString name = streamName(uri);
    if (name.isEmpty() || m_links.count(name) != 0) {
        return {};
    }
    if (!permitted(true, false)) {
        return tr("QtTune needs the Nearby devices permission to reach "
                  "Bluetooth adapters: allow it, then connect again");
    }
    const bool ble = name.startsWith(QLatin1String("ble:"));
    const QString address = name.mid(name.indexOf(QLatin1Char(':')) + 1);
    auto* link = new BtLink(
        ble ? BtLink::Kind::LowEnergy : BtLink::Kind::Classic, address, this);
    const QString refused = link->registerAs(name);
    if (!refused.isEmpty()) {
        delete link;
        return refused;
    }
    m_links[name] = link;
    return {};
}

#endif
