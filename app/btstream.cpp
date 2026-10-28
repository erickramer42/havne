// File/layer: app/btstream.cpp | Qt6 Core/Bluetooth, app layer, C++17;
//             built on Android only (QTTUNE_BLUETOOTH)
// Testing: CI's android-build compiles it; BENCH and Vehicle on the phone
//          (ADR-0028 §4); the BLE choice is UNIT-tested (blepicker.h)

#include "btstream.h"
#include "blepicker.h"

#include <QBluetoothDeviceInfo>
#include <QBluetoothSocket>
#include <QBluetoothUuid>
#include <QLowEnergyController>
#include <QLowEnergyDescriptor>
#include <QMetaObject>

#include <algorithm>

namespace {

// A BLE write before the MTU is negotiated carries 20 bytes (23 less 3)
constexpr int kMinBleWrite = 20;

QString uuidText(const QBluetoothUuid& uuid)
{
    return bleUuid(uuid.toString(QUuid::WithoutBraces));
}

} // namespace

BtLink::BtLink(Kind kind, const QString& address, QObject* parent)
    : QObject(parent), m_kind(kind), m_address(address)
{}

BtLink::~BtLink()
{
    teardown();
    if (m_stream != nullptr) {
        (void)ecuio_stream_unregister(m_stream);
    }
}

QString BtLink::registerAs(const QString& name)
{
    ecuio_stream_callbacks_t cb{};
    cb.open = &BtLink::onOpen;
    cb.write = &BtLink::onWrite;
    cb.close = &BtLink::onClose;
    cb.user = this;
    const QByteArray name8 = name.toUtf8();
    const ecuio_status_t st =
        ecuio_stream_register(name8.constData(), &cb, &m_stream);
    if (st != ECUIO_OK) {
        m_stream = nullptr;
        return tr("%1 can't be a link: %2")
            .arg(name, QString::fromUtf8(ecuio_status_string(st)));
    }
    return {};
}

// Core's three calls return at once: the work is queued to this object's
// thread, in the order core made them. A call still queued when the
// object goes is dropped with it.
void BtLink::onOpen(void* user)
{
    auto* self = static_cast<BtLink*>(user);
    QMetaObject::invokeMethod(
        self, [self] { self->open(); }, Qt::QueuedConnection);
}

void BtLink::onWrite(void* user, const uint8_t* data, uint32_t len)
{
    auto* self = static_cast<BtLink*>(user);
    const QByteArray bytes(reinterpret_cast<const char*>(data),
                           static_cast<qsizetype>(len));
    QMetaObject::invokeMethod(
        self, [self, bytes] { self->write(bytes); }, Qt::QueuedConnection);
}

void BtLink::onClose(void* user)
{
    auto* self = static_cast<BtLink*>(user);
    QMetaObject::invokeMethod(
        self, [self] { self->close(); }, Qt::QueuedConnection);
}

void BtLink::open()
{
    teardown();
    m_opening = true;
    if (m_kind == Kind::Classic) {
        openClassic();
    } else {
        openLowEnergy();
    }
}

void BtLink::write(const QByteArray& bytes)
{
    if (!m_up) {
        return; // core sees the failure it was told of
    }
    if (m_socket != nullptr) {
        if (m_socket->write(bytes) != bytes.size()) {
            fail(tr("the write failed: %1").arg(m_socket->errorString()));
        }
        return;
    }
    if (m_service != nullptr) {
        const int chunk = std::max(kMinBleWrite, m_controller->mtu() - 3);
        for (qsizetype at = 0; at < bytes.size(); at += chunk) {
            m_service->writeCharacteristic(m_writeTo, bytes.mid(at, chunk),
                                           m_writeMode);
        }
    }
}

void BtLink::close()
{
    m_opening = m_up = false;
    teardown();
}

void BtLink::opened()
{
    if (!m_opening) {
        return;
    }
    m_opening = false;
    m_up = true;
    ecuio_stream_opened(m_stream);
}

// Once per open: core closes, then opens again on its backoff
void BtLink::fail(const QString& reason)
{
    if (!m_opening && !m_up) {
        return;
    }
    m_opening = m_up = false;
    const QByteArray why = reason.toUtf8();
    ecuio_stream_failed(m_stream, why.constData());
    teardown();
}

// The link's objects go, their signals cut first: one may be emitting
void BtLink::teardown()
{
    if (m_socket != nullptr) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_controller != nullptr) {
        for (QLowEnergyService* s : m_services) {
            s->disconnect(this);
        }
        m_controller->disconnect(this);
        m_controller->disconnectFromDevice();
        m_controller->deleteLater(); // the services with it
        m_controller = nullptr;
    }
    m_services.clear();
    m_undiscovered = 0;
    m_service = nullptr;
    m_writeTo = {};
    m_notifyFrom = {};
}

// ---- Classic: RFCOMM to the SPP service ---------------------------------

void BtLink::openClassic()
{
    m_socket =
        new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol, this);
    connect(m_socket, &QBluetoothSocket::connected, this, &BtLink::opened);
    connect(m_socket, &QBluetoothSocket::readyRead, this, [this] {
        const QByteArray bytes = m_socket->readAll();
        if (m_up && !bytes.isEmpty()) {
            ecuio_stream_received(
                m_stream, reinterpret_cast<const uint8_t*>(bytes.constData()),
                static_cast<uint32_t>(bytes.size()));
        }
    });
    connect(m_socket, &QBluetoothSocket::errorOccurred, this,
            [this](QBluetoothSocket::SocketError) {
                fail(m_socket->errorString());
            });
    connect(m_socket, &QBluetoothSocket::disconnected, this,
            [this] { fail(tr("the adapter disconnected")); });
    m_socket->connectToService(
        m_address,
        QBluetoothUuid(QBluetoothUuid::ServiceClassUuid::SerialPort));
}

// ---- BLE: the GATT service pickBleService chooses -----------------------

void BtLink::openLowEnergy()
{
    QBluetoothDeviceInfo info(m_address, QString(), 0);
    info.setCoreConfigurations(
        QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
    m_controller = QLowEnergyController::createCentral(info, this);
    connect(m_controller, &QLowEnergyController::connected, this,
            [this] { m_controller->discoverServices(); });
    connect(m_controller, &QLowEnergyController::discoveryFinished, this,
            &BtLink::servicesFound);
    connect(m_controller, &QLowEnergyController::errorOccurred, this,
            [this](QLowEnergyController::Error) {
                fail(m_controller->errorString());
            });
    connect(m_controller, &QLowEnergyController::disconnected, this,
            [this] { fail(tr("the adapter disconnected")); });
    m_controller->connectToDevice();
}

// Every service's characteristics, so the picker sees them all
void BtLink::servicesFound()
{
    for (const QBluetoothUuid& uuid : m_controller->services()) {
        QLowEnergyService* s =
            m_controller->createServiceObject(uuid, m_controller);
        if (s == nullptr) {
            continue;
        }
        m_services.push_back(s);
        connect(s, &QLowEnergyService::stateChanged, this,
                [this](QLowEnergyService::ServiceState state) {
                    if (state == QLowEnergyService::RemoteServiceDiscovered) {
                        serviceDiscovered();
                    }
                });
        connect(s, &QLowEnergyService::errorOccurred, this,
                [this](QLowEnergyService::ServiceError) {
                    fail(tr("the adapter's services couldn't be read"));
                });
    }
    m_undiscovered = static_cast<int>(m_services.size());
    if (m_undiscovered == 0) {
        fail(tr("the adapter offers no services"));
        return;
    }
    for (QLowEnergyService* s : m_services) {
        s->discoverDetails(QLowEnergyService::SkipValueDiscovery);
    }
}

void BtLink::serviceDiscovered()
{
    if (--m_undiscovered > 0) {
        return;
    }
    std::vector<BleService> seen;
    for (QLowEnergyService* s : m_services) {
        BleService service;
        service.uuid = uuidText(s->serviceUuid());
        for (const QLowEnergyCharacteristic& c : s->characteristics()) {
            const auto p = c.properties();
            BleCharacteristic ch;
            ch.uuid = uuidText(c.uuid());
            ch.write = p.testFlag(QLowEnergyCharacteristic::Write);
            ch.writeNoResponse =
                p.testFlag(QLowEnergyCharacteristic::WriteNoResponse);
            ch.notify = p.testFlag(QLowEnergyCharacteristic::Notify);
            ch.indicate = p.testFlag(QLowEnergyCharacteristic::Indicate);
            service.characteristics.push_back(ch);
        }
        seen.push_back(service);
    }
    const BlePick pick = pickBleService(seen);
    if (!pick.found()) {
        fail(tr("the adapter has no service that carries ELM327 text"));
        return;
    }
    m_service = m_services[size_t(pick.service)];
    const QList<QLowEnergyCharacteristic> chars = m_service->characteristics();
    m_writeTo = chars[pick.write];
    m_notifyFrom = chars[pick.notify];
    m_writeMode = m_writeTo.properties().testFlag(
                      QLowEnergyCharacteristic::WriteNoResponse)
                      ? QLowEnergyService::WriteWithoutResponse
                      : QLowEnergyService::WriteWithResponse;
    subscribe();
}

// Notifications (or indications) on, then the link is up
void BtLink::subscribe()
{
    connect(m_service, &QLowEnergyService::characteristicChanged, this,
            [this](const QLowEnergyCharacteristic& c, const QByteArray& value) {
                if (m_up && c.uuid() == m_notifyFrom.uuid() &&
                    !value.isEmpty()) {
                    ecuio_stream_received(
                        m_stream,
                        reinterpret_cast<const uint8_t*>(value.constData()),
                        static_cast<uint32_t>(value.size()));
                }
            });
    const QLowEnergyDescriptor cccd = m_notifyFrom.descriptor(
        QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
    if (!cccd.isValid()) {
        opened();
        return;
    }
    connect(
        m_service, &QLowEnergyService::descriptorWritten, this,
        [this](const QLowEnergyDescriptor&, const QByteArray&) { opened(); });
    const bool notify =
        m_notifyFrom.properties().testFlag(QLowEnergyCharacteristic::Notify);
    m_service->writeDescriptor(
        cccd, notify ? QLowEnergyCharacteristic::CCCDEnableNotification
                     : QLowEnergyCharacteristic::CCCDEnableIndication);
}
