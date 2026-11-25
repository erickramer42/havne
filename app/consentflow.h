// File/layer: app/consentflow.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.AConnectWaitsForConsent* and the other
//          consent and unlock cases (tests/test_bridge_consent.cpp)
//
// The conversation tier's front in the app (ADR-0066's appendix of
// 2026-11-23), QML's Bridge.consent. At a connect whose plugin's code
// talks to modules over a car's transport (elm327://, j2534://), the
// bridge asks needFor before the connect thread, which runs applyFound
// after create: a record in the consent store is applied there, else the
// session waits unstarted while ask shows the consent screen. Allow gives
// (the store's give, then apply), Decline or the limit (120 s) start it
// as data only, and a Decline is remembered by fingerprint until the app
// quits. Settings lists the records and withdraws them; the Diagnostics
// page reads each talking module's report (conversations) and gives the
// word for one unlock (unlock).
//
// Every store open, give, withdraw, apply and unlock runs on one worker
// thread of this object's, in the order posted; the GUI thread never
// waits on the store's lock. The bridge drains the worker before a
// session's close and joins it before its destructor destroys a session.

#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "ecuio/core.h"
#include "ecuio/plugin.h"

class PluginStore;
class QTimer;

// What a connect's plugin asks, read on the GUI thread before the
// connect thread runs
struct ConsentNeed {
    bool talks = false;    // the plugin's code talks to modules
    bool asks = false;     // a car's transport and a publisher: consent asked
    bool declined = false; // declined earlier in this run
    QByteArray fingerprint; // 32 bytes
    QByteArray termsSha;    // 32 bytes, "" with no publisher
    QString sha;            // the store's copy
    QString id;
    QString name;
    QString version;
    QString publisher;
    QString consentPath;
    QString securityPath;
    QByteArray label;      // the vehicle's, the security store's key
    QVariantMap screen;    // the consent screen's facts (ConsentFlow::ask)
    QVariantList ecus;     // { ecu, rx, levels [{ level, delayMs }] }
};

// What the connect thread found in the stores
struct ConsentFound {
    bool waits = false;      // no record: the session waits for an answer
    QStringList lines;       // for the bridge's log
    QVariantMap changed;     // a version change's facts, {} for none
};

class ConsentFlow : public QObject
{
    Q_OBJECT
    // The consent screen while a connect waits, {} otherwise: { id, name,
    // version, set, classes, lines [{ id, key, heading, text, action }],
    // publisher { name, contact, license }, terms, fingerprint (64),
    // fingerprintShort (16 in four groups), changed ({} or { version,
    // givenMs, publisher, publisherSame, termsSame }), limitMs }
    Q_PROPERTY(QVariantMap ask READ ask NOTIFY askChanged)
    Q_PROPERTY(bool waiting READ waiting NOTIFY askChanged)
    // The store's records, newest first: { id, version, publisher,
    // fingerprint, givenMs }; refresh() reads them
    Q_PROPERTY(QVariantList records READ records NOTIFY recordsChanged)
    // A store change or read under way
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // One entry for each channel a plugin's code talks on: { channel, ecu,
    // rx, state, unlock, level, nrc, failed, delayLeftMs, delayEndsMs,
    // kept, levels [{ level, delayMs }] }; state and unlock are words
    // ("talking", "unlocked"), levels the manifest's for that module
    Q_PROPERTY(QVariantList conversations READ conversations NOTIFY conversationsChanged)

public:
    static constexpr int LimitMs = 120000;

    explicit ConsentFlow(PluginStore* store, QObject* parent = nullptr);
    ~ConsentFlow() override;

    void setLog(std::function<void(const QString&)> log)
    {
        m_log = std::move(log);
    }
    // The store files' folder rule, the plugin store's: consent.json and
    // security.json beside its plugins folder
    static QString storePath(const char* name);

    // GUI thread, before the connect thread
    ConsentNeed needFor(const QString& uri, const QString& vehicleLabel);
    // The connect thread, after create: names the security store and
    // applies a record found; never waits
    static ConsentFound applyFound(ecuio_session_t* session,
                                   const ConsentNeed& need);
    // GUI thread: the session waits unstarted for the answer; done runs
    // on the GUI thread once it is given (applied or not)
    void wait(ecuio_session_t* session, const ConsentNeed& need,
              const ConsentFound& found, std::function<void()> done);
    // GUI thread: a started session's modules, and its end
    void begin(ecuio_session_t* session, const ConsentNeed& need);
    void poll(uint32_t channels);
    void end();
    // Any thread: returns once every job posted has run
    void drain();
    // GUI thread: runs what is posted, then stops the worker
    void join();

    QVariantMap ask() const { return m_ask; }
    bool waiting() const { return m_waitSession != nullptr; }
    QVariantList records() const { return m_records; }
    bool busy() const { return m_jobsOut > 0; }
    QVariantList conversations() const { return m_conversations; }

    // Allow (true) or Decline (false) for the connect waiting
    Q_INVOKABLE void answer(bool allow);
    // The screen for the plugin with copy sha, outside a connect
    // (Settings' Review...); {} for none or a plugin asking no consent
    Q_INVOKABLE QVariantMap screenFor(const QString& sha);
    // Allow from that screen: the store's give; answered by given()
    Q_INVOKABLE void give(const QString& sha);
    Q_INVOKABLE void refresh();
    // fingerprint (64 hex) "" withdraws every record of the id
    Q_INVOKABLE void withdraw(const QString& id, const QString& fingerprint);
    // The words shown before an unlock: [{ key, heading, text }]
    Q_INVOKABLE QVariantList unlockLines() const;
    // The user's word for one attempt at level on channel's module:
    // { ok, words }; ok means asked of core, and unlockAnswered follows
    Q_INVOKABLE QVariantMap unlock(int channel, int level);

    // Seams, never called from app code
    void setLimitForTesting(int ms) { m_limitMs = ms; }
    Qt::HANDLE unlockThreadForTesting() const { return m_unlockThread.load(); }

signals:
    void askChanged();
    void recordsChanged();
    void busyChanged();
    void conversationsChanged();
    void given(bool ok, const QString& words);
    void withdrawn(bool ok, const QString& words);
    void unlockAnswered(int channel, bool ok, const QString& words);

private:
    void post(std::function<void()> job);
    void work();
    void log(const QString& line);
    void decline(const QString& line);
    void finishWait(const QString& line);
    void jobDone();
    QVariantMap readScreen(const ecuio_plugin_t* plugin) const;

    PluginStore* m_store;
    std::function<void(const QString&)> m_log;
    QTimer* m_limit;
    int m_limitMs = LimitMs;
    QSet<QByteArray> m_declined;
    // The connect waiting
    ecuio_session_t* m_waitSession = nullptr;
    ConsentNeed m_waitNeed;
    std::function<void()> m_waitDone;
    bool m_answering = false;
    QVariantMap m_ask;
    QVariantList m_records;
    int m_jobsOut = 0;
    // The started session's
    ecuio_session_t* m_session = nullptr;
    QVariantList m_ecus;
    bool m_vehicleNamed = false; // the security store's label was given
    uint32_t m_generations[ECUIO_MAX_CHANNELS] = {};
    bool m_asking[ECUIO_MAX_CHANNELS] = {};
    QVariantList m_conversations;
    QString m_securityPath;
    std::atomic<Qt::HANDLE> m_unlockThread{nullptr};
    // The worker
    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_jobs;
    bool m_running = false;
    bool m_stop = false;
};
