// File/layer: app/adapterrelay.cpp | Qt6 Core/Network, app layer, C++17;
//             every platform
// Testing: UNIT: BridgeTest.Relay* (test_bridge); BENCH and Vehicle: the
//          phone with the MX+ (ADR-0047)

#include "adapterrelay.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

AdapterRelay::AdapterRelay(QObject* parent) : QObject(parent) {}

AdapterRelay::~AdapterRelay()
{
    stop();
}

int AdapterRelay::port() const
{
    return m_server != nullptr ? m_server->serverPort() : 0;
}

QString AdapterRelay::start(const QString& uri, int port)
{
    if (running()) {
        return tr("A relay is already running");
    }
    if (!available()) {
        return tr("This build can't relay an adapter: the phone's app can");
    }
    // elm327://bt:<address>[?...]: a paired classic adapter, whose link
    // is one socket
    const QString scheme = QStringLiteral("elm327://");
    QString rest = uri.trimmed();
    if (!rest.startsWith(scheme, Qt::CaseInsensitive)) {
        return tr("A relay takes a paired Bluetooth adapter's URI, "
                  "elm327://bt:<address>: select one in the list");
    }
    rest = rest.mid(scheme.size());
    const qsizetype query = rest.indexOf(QLatin1Char('?'));
    if (query >= 0) {
        rest.truncate(query);
    }
    if (rest.startsWith(QLatin1String("ble:"), Qt::CaseInsensitive)) {
        return tr("A relay takes a classic Bluetooth adapter (bt:), not a "
                  "BLE one");
    }
    if (!rest.startsWith(QLatin1String("bt:"), Qt::CaseInsensitive) ||
        rest.size() <= 3) {
        return tr("A relay takes a paired Bluetooth adapter's URI, "
                  "elm327://bt:<address>: select one in the list");
    }
    if (port < 0 || port > 65535) {
        return tr("The port must be 1 to 65535");
    }
    std::unique_ptr<RelayAdapter> adapter = m_factory(rest.mid(3));
    if (adapter == nullptr) {
        return tr("The adapter %1 can't be relayed").arg(rest);
    }
    auto* server = new QTcpServer(this);
    // The phone's own loopback address alone: adb's forward reaches it,
    // and nothing on a network does
    if (!server->listen(QHostAddress::LocalHost, static_cast<quint16>(port))) {
        const QString why = server->errorString();
        delete server;
        return tr("Port %1 can't be listened on: %2").arg(port).arg(why);
    }
    m_server = server;
    connect(m_server, &QTcpServer::newConnection, this, &AdapterRelay::accept);
    m_adapter = std::move(adapter);
    m_adapter->opened = [this] { adapterOpened(); };
    m_adapter->received = [this](const QByteArray& b) { adapterReceived(b); };
    m_adapter->failed = [this](const QString& why) { adapterFailed(why); };
    m_uri = uri.trimmed();
    m_bytesIn = m_bytesOut = 0;
    emit runningChanged();
    emit trafficChanged();
    openAdapter();
    return {};
}

void AdapterRelay::stop()
{
    if (!running()) {
        return;
    }
    dropComputer();
    m_server->close();
    m_server->deleteLater();
    m_server = nullptr;
    // Its callbacks first: a link closing may report its end
    m_adapter->opened = nullptr;
    m_adapter->received = nullptr;
    m_adapter->failed = nullptr;
    m_adapter->close();
    m_adapter.reset();
    m_opening = m_up = false;
    m_uri.clear();
    emit runningChanged();
    setStatus(tr("Stopped"));
}

void AdapterRelay::openAdapter()
{
    if (m_opening || m_up) {
        return;
    }
    m_opening = true;
    setStatus(tr("Opening the adapter"));
    m_adapter->open();
}

// One computer at a time: a second is closed at once
void AdapterRelay::accept()
{
    while (QTcpSocket* s = m_server->nextPendingConnection()) {
        if (m_client != nullptr) {
            s->abort();
            s->deleteLater();
            continue;
        }
        m_client = s;
        connect(s, &QTcpSocket::readyRead, this, &AdapterRelay::computerRead);
        connect(s, &QTcpSocket::disconnected, this,
                &AdapterRelay::computerGone);
        if (m_up) {
            setStatus(tr("Relaying to a computer"));
        } else {
            openAdapter();
            setStatus(tr("A computer is connected; opening the adapter"));
        }
    }
}

void AdapterRelay::computerRead()
{
    if (m_client == nullptr) {
        return;
    }
    const QByteArray bytes = m_client->readAll();
    if (bytes.isEmpty()) {
        return;
    }
    if (!m_up) {
        if (m_held.size() + bytes.size() > kHeldMax) {
            dropComputer();
            setStatus(tr("The computer sent more than %1 bytes before the "
                         "adapter opened, and was disconnected")
                          .arg(kHeldMax));
            return;
        }
        m_held += bytes;
        return;
    }
    m_adapter->write(bytes);
    m_bytesOut += bytes.size();
    emit trafficChanged();
}

// The computer left: the adapter's link stays open for the next
void AdapterRelay::computerGone()
{
    dropComputer();
    setStatus(waiting());
}

void AdapterRelay::dropComputer()
{
    m_held.clear();
    if (m_client == nullptr) {
        return;
    }
    QTcpSocket* s = m_client;
    m_client = nullptr;
    s->disconnect(this);
    s->abort();
    s->deleteLater();
}

void AdapterRelay::adapterOpened()
{
    m_opening = false;
    m_up = true;
    if (m_client == nullptr) {
        setStatus(waiting());
        return;
    }
    if (!m_held.isEmpty()) {
        m_adapter->write(m_held);
        m_bytesOut += m_held.size();
        m_held.clear();
        emit trafficChanged();
    }
    setStatus(tr("Relaying to a computer"));
}

void AdapterRelay::adapterReceived(const QByteArray& bytes)
{
    // An adapter's bytes with no computer to take them answer nothing
    // anyone asked
    if (m_client == nullptr || bytes.isEmpty()) {
        return;
    }
    m_client->write(bytes);
    m_bytesIn += bytes.size();
    emit trafficChanged();
}

// The computer's link ends with the adapter's, so its core sees a failed
// port and connects again on its backoff, opening the adapter again
void AdapterRelay::adapterFailed(const QString& why)
{
    const bool was_up = m_up;
    m_opening = m_up = false;
    dropComputer();
    setStatus(was_up ? tr("The adapter disconnected: %1. It opens again when "
                          "a computer connects")
                           .arg(why)
                     : tr("The adapter didn't open: %1. It opens again when "
                          "a computer connects")
                           .arg(why));
}

// Where it stands with no computer connected
QString AdapterRelay::waiting() const
{
    if (m_opening) {
        return tr("Opening the adapter");
    }
    if (m_up) {
        return tr("The adapter is open; waiting for a computer on port %1 "
                  "(adb forward tcp:%1 tcp:%1, then elm327://127.0.0.1:%1)")
            .arg(port());
    }
    return tr("Waiting for a computer on port %1; the adapter opens when one "
              "connects")
        .arg(port());
}

void AdapterRelay::setStatus(const QString& status)
{
    if (status == m_status) {
        return;
    }
    m_status = status;
    emit statusChanged();
}
