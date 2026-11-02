// File/layer: app/adapterrelay.h | Qt6 Core/Network, app layer, C++17;
//             every platform (its Bluetooth adapter, btrelay.cpp, is
//             Android's)
// Testing: UNIT: BridgeTest.Relay* (test_bridge, a fake adapter and real
//          loopback TCP sockets): bytes both ways in order, a computer's
//          bytes held until the adapter opens and too many refused, a
//          second computer refused, the adapter's loss ending the
//          computer's link, the computer's leaving keeping the adapter,
//          stop, the URI's refusals; BENCH and Vehicle: the phone with
//          the MX+ (ADR-0047)
//
// One adapter relayed to a computer (ADR-0047): the phone holds the
// adapter's link and listens on its own loopback address, and a computer
// reaches it over USB through adb's forward (adb forward tcp:35000
// tcp:35000), where havne-cli connects as to a WiFi adapter,
// elm327://127.0.0.1:35000. The relay passes bytes and reads none: the
// computer's core runs the ELM327 setup, the requests and the wire log.
// One computer at a time; the adapter's link opens at start and stays
// open between computers, and one that drops opens again when the next
// computer connects. Everything runs on the relay's thread (the GUI's).

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QTcpServer;
class QTcpSocket;

// The adapter's side: a link opened and closed on request, its outcome
// and its bytes through the callbacks, on the relay's thread. The relay
// sets the callbacks before its first call.
class RelayAdapter
{
public:
    virtual ~RelayAdapter() = default;
    // Ends in opened() or failed(); while open or opening, nothing
    virtual void open() = 0;
    virtual void write(const QByteArray& bytes) = 0;
    virtual void close() = 0;

    std::function<void()> opened;
    std::function<void(const QByteArray&)> received;
    // The open failed, or the open link dropped: why, in words
    std::function<void(const QString&)> failed;
};

class AdapterRelay : public QObject
{
    Q_OBJECT
    // This build can relay (the phone's, with Bluetooth)
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(int defaultPort READ defaultPort CONSTANT)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    // What's relayed (the URI start was given), "" while stopped
    Q_PROPERTY(QString adapterUri READ adapterUri NOTIFY runningChanged)
    // The port it listens on, 0 while stopped
    Q_PROPERTY(int port READ port NOTIFY runningChanged)
    Q_PROPERTY(bool computerConnected READ computerConnected NOTIFY statusChanged)
    // Where it stands, in a sentence
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // Bytes from the adapter to the computer, and the other way
    Q_PROPERTY(qint64 bytesIn READ bytesIn NOTIFY trafficChanged)
    Q_PROPERTY(qint64 bytesOut READ bytesOut NOTIFY trafficChanged)

public:
    // The adapter for a classic Bluetooth address
    using Factory =
        std::function<std::unique_ptr<RelayAdapter>(const QString& address)>;

    static constexpr int kDefaultPort = 35000;
    // A computer's bytes held while the adapter opens: an ELM327 setup's
    // first commands, a few dozen; past this the computer isn't one
    static constexpr int kHeldMax = 4096;

    explicit AdapterRelay(QObject* parent = nullptr);
    ~AdapterRelay() override;

    // main()'s on Android, set before the engine loads QML; without one
    // the QML singleton is the engine's own, with no factory
    inline static AdapterRelay* instance = nullptr;

    // Without one, available is false and start refuses
    void setFactory(Factory factory) { m_factory = std::move(factory); }

    bool available() const { return static_cast<bool>(m_factory); }
    int defaultPort() const { return kDefaultPort; }
    bool running() const { return m_server != nullptr; }
    QString adapterUri() const { return m_uri; }
    int port() const;
    bool computerConnected() const { return m_client != nullptr; }
    QString status() const { return m_status; }
    qint64 bytesIn() const { return m_bytesIn; }
    qint64 bytesOut() const { return m_bytesOut; }

    // Relays the adapter uri names, elm327://bt:<address> (its query
    // ignored), on 127.0.0.1 at port (0: one the system picks), and opens
    // its link. "" once listening, else why not
    Q_INVOKABLE QString start(const QString& uri, int port = kDefaultPort);
    // Ends the computer's link, the listening and the adapter's link
    Q_INVOKABLE void stop();

signals:
    void runningChanged();
    void statusChanged();
    void trafficChanged();

private:
    void accept();
    void computerRead();
    void computerGone();
    void dropComputer();
    void adapterOpened();
    void adapterReceived(const QByteArray& bytes);
    void adapterFailed(const QString& why);
    void openAdapter();
    void setStatus(const QString& status);
    QString waiting() const;

    Factory m_factory;
    std::unique_ptr<RelayAdapter> m_adapter;
    QTcpServer* m_server = nullptr;
    QTcpSocket* m_client = nullptr;
    QString m_uri;
    QString m_status;
    QByteArray m_held;       // the computer's, until the adapter opens
    bool m_opening = false;  // the adapter's link is opening
    bool m_up = false;       // and is open
    qint64 m_bytesIn = 0;
    qint64 m_bytesOut = 0;
};
