// File/layer: app/consentflow.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest consent and unlock cases
//          (tests/test_bridge_consent.cpp, over the fake J2534 DLL)

#include "consentflow.h"

#include "bridgeformats.h"
#include "pluginstore.h"

#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <iterator>
#include <utility>

namespace {

QString hexOf(const uint8_t* bytes, size_t n)
{
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(bytes),
                   static_cast<qsizetype>(n))
            .toHex());
}

// The fingerprint's first 16 digits in four groups, as the screen shows it
QString shortPrint(const QByteArray& print)
{
    const QString hex = QString::fromLatin1(print.left(8).toHex());
    return QStringLiteral("%1 %2 %3 %4")
        .arg(hex.mid(0, 4), hex.mid(4, 4), hex.mid(8, 4), hex.mid(12, 4));
}

QString filled(const char* text, const QString& name, const QString& version)
{
    QString out = QString::fromUtf8(text);
    for (const auto& [field, value] :
         {std::make_pair(QStringLiteral("{name}"), name),
          std::make_pair(QStringLiteral("{version}"), version)}) {
        const qsizetype at = out.indexOf(field);
        if (at >= 0) {
            out.replace(at, field.size(), value);
        }
    }
    return out;
}

QString detail()
{
    return QString::fromUtf8(ecuio_last_error_detail());
}

const char* const kStates[] = {"none",     "unbegun", "talking",
                               "declined", "faulted", "ended"};
const char* const kUnlocks[] = {
    "none",   "waiting", "seed",       "key",        "unlocked",
    "failed", "ended",   "lost",       "noLevel",    "keyTooLong",
    "busy",   "delayed", "alreadyUnlocked", "refused"};

QString wordOf(const char* const* words, size_t count, uint8_t value)
{
    return value < count ? QString::fromLatin1(words[value]) : QString();
}

// The module's ECUs and the levels the manifest declares for each
QVariantList ecusOf(const ecuio_plugin_t* plugin)
{
    ecuio_plugin_conversation_t c{};
    c.struct_size = sizeof c;
    QVariantList out;
    if (ecuio_plugin_conversation(plugin, &c) != ECUIO_OK) {
        return out;
    }
    for (uint32_t i = 0; i < c.ecu_count; ++i) {
        ecuio_plugin_conversation_ecu_t e{};
        e.struct_size = sizeof e;
        if (ecuio_plugin_conversation_ecu(plugin, i, &e) != ECUIO_OK) {
            continue;
        }
        QVariantList levels;
        for (uint32_t k = 0; k < c.level_count; ++k) {
            ecuio_plugin_security_level_t l{};
            l.struct_size = sizeof l;
            if (ecuio_plugin_security_level(plugin, k, &l) == ECUIO_OK &&
                l.rx_id == e.rx_id) {
                levels << QVariantMap{
                    {QStringLiteral("level"), int{l.level}},
                    {QStringLiteral("delayMs"), qint64{l.delay_ms}}};
            }
        }
        out << QVariantMap{{QStringLiteral("ecu"), QString::fromUtf8(e.id)},
                           {QStringLiteral("rx"), qint64{e.rx_id}},
                           {QStringLiteral("levels"), levels}};
    }
    return out;
}

// The packed copy at path loaded (no profile registered); nullptr with
// *words saying why not
ecuio_plugin_t* loadCopy(const QString& path, QString* words)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *words = QStringLiteral("the plugin's copy doesn't read");
        return nullptr;
    }
    const QByteArray bytes = f.readAll();
    ecuio_plugin_t* plugin = nullptr;
    if (ecuio_plugin_load(bytes.constData(), static_cast<size_t>(bytes.size()),
                          nullptr, nullptr, &plugin) != ECUIO_OK) {
        *words = detail();
        return nullptr;
    }
    return plugin;
}

QByteArray printOf(const ecuio_plugin_t* plugin)
{
    ecuio_plugin_fingerprint_t f{};
    f.struct_size = sizeof f;
    if (ecuio_plugin_fingerprint(plugin, &f) != ECUIO_OK) {
        return {};
    }
    return QByteArray(reinterpret_cast<const char*>(f.fingerprint),
                      sizeof f.fingerprint);
}

struct Given {
    bool ok = false;    // given, and applied when a session was named
    bool given = false; // the record saved
    QString words;
    QByteArray print;
};

// The worker: the store's give of the copy at path (its fingerprint must
// be expected, when one is named), then the apply to session (none
// outside a connect)
Given giveAndApply(const QString& path, const QString& storePath,
                   const QByteArray& expected, ecuio_session_t* session)
{
    Given g;
    ecuio_plugin_t* plugin = loadCopy(path, &g.words);
    if (plugin == nullptr) {
        return g;
    }
    g.print = printOf(plugin);
    if (!expected.isEmpty() && g.print != expected) {
        g.words = QStringLiteral("the plugin changed since it was shown");
        (void)ecuio_plugin_free(plugin);
        return g;
    }
    ecuio_consent_store_t* store = nullptr;
    ecuio_status_t rc =
        ecuio_consent_store_open(storePath.toUtf8().constData(), &store);
    if (rc == ECUIO_OK) {
        rc = ecuio_consent_store_give(
            store, plugin,
            static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch()));
    }
    g.given = rc == ECUIO_OK;
    if (rc == ECUIO_OK && session != nullptr) {
        rc = ecuio_session_apply_consent(
            session, store, reinterpret_cast<const uint8_t*>(g.print.constData()));
    }
    g.ok = rc == ECUIO_OK;
    if (!g.ok) {
        g.words = detail(); // read on this thread, right after the call
    }
    if (store != nullptr) {
        ecuio_consent_store_close(store);
    }
    (void)ecuio_plugin_free(plugin);
    return g;
}

QVariantMap recordMap(const ecuio_consent_record_t& r)
{
    return {{QStringLiteral("id"), QString::fromUtf8(r.id)},
            {QStringLiteral("version"), QString::fromUtf8(r.version)},
            {QStringLiteral("publisher"), QString::fromUtf8(r.publisher)},
            {QStringLiteral("fingerprint"), hexOf(r.fingerprint, 32)},
            {QStringLiteral("givenMs"),
             static_cast<qint64>(r.given_utc) * 1000}};
}

} // namespace

ConsentFlow::ConsentFlow(PluginStore* store, QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_limit(new QTimer(this))
{
    m_limit->setSingleShot(true);
    connect(m_limit, &QTimer::timeout, this, [this]() {
        decline(QStringLiteral("No answer in %1 s: %2 reads signals only "
                               "until Havne quits")
                    .arg(m_limitMs / 1000)
                    .arg(m_waitNeed.name));
    });
}

ConsentFlow::~ConsentFlow()
{
    join();
}

QString ConsentFlow::storePath(const char* name)
{
    const QString root = PluginStore::root();
    return root.left(root.lastIndexOf(QLatin1Char('/')) + 1) +
           QString::fromLatin1(name);
}

QVariantMap ConsentFlow::readScreen(const ecuio_plugin_t* plugin) const
{
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    ecuio_plugin_publisher_t pub{};
    pub.struct_size = sizeof pub;
    size_t n = 0;
    if (ecuio_plugin_info(plugin, &info) != ECUIO_OK ||
        ecuio_plugin_publisher(plugin, &pub) != ECUIO_OK ||
        ecuio_plugin_terms(plugin, nullptr, 0, &n) == ECUIO_ERR_NULL_ARGUMENT) {
        return {};
    }
    QByteArray terms(static_cast<qsizetype>(n) + 1, '\0');
    if (ecuio_plugin_terms(plugin, terms.data(),
                           static_cast<size_t>(terms.size()), &n) !=
        ECUIO_OK) {
        return {};
    }
    terms.truncate(static_cast<qsizetype>(n));
    const QString name = QString::fromUtf8(info.name);
    const QString version = QString::fromUtf8(info.version);
    const uint32_t set = ECUIO_CONSENT_SET_CONVERSATION;
    QVariantList lines;
    ecuio_consent_line_t l{};
    l.struct_size = sizeof l;
    for (uint32_t i = 0; ecuio_consent_line(set, i, &l) == ECUIO_OK; ++i) {
        lines << QVariantMap{
            {QStringLiteral("id"), qint64{l.id}},
            {QStringLiteral("key"), QString::fromUtf8(l.key)},
            {QStringLiteral("heading"), QString::fromUtf8(l.heading)},
            {QStringLiteral("text"), filled(l.text, name, version)},
            {QStringLiteral("action"),
             (l.flags & ECUIO_CONSENT_LINE_ACTION) != 0}};
    }
    const QByteArray print = printOf(plugin);
    return {{QStringLiteral("id"), QString::fromUtf8(info.id)},
            {QStringLiteral("name"), name},
            {QStringLiteral("version"), version},
            {QStringLiteral("set"), qint64{set}},
            {QStringLiteral("classes"), QStringList{QStringLiteral("conversation")}},
            {QStringLiteral("lines"), lines},
            {QStringLiteral("publisher"),
             QVariantMap{{QStringLiteral("name"), QString::fromUtf8(pub.name)},
                         {QStringLiteral("contact"), QString::fromUtf8(pub.contact)},
                         {QStringLiteral("license"), QString::fromUtf8(pub.license)}}},
            {QStringLiteral("terms"), QString::fromUtf8(terms)},
            {QStringLiteral("fingerprint"), QString::fromLatin1(print.toHex())},
            {QStringLiteral("fingerprintShort"), shortPrint(print)},
            {QStringLiteral("changed"), QVariantMap{}},
            {QStringLiteral("limitMs"), m_limitMs}};
}

ConsentNeed ConsentFlow::needFor(const QString& uri,
                                 const QString& vehicleLabel)
{
    ConsentNeed n;
    const ecuio_plugin_t* plugin = m_store->pluginOfProfile(
        BridgeFormats::profileOfUri(uri), &n.sha);
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    if (plugin == nullptr || ecuio_plugin_info(plugin, &info) != ECUIO_OK ||
        (info.tiers & ECUIO_PLUGIN_TIER_CONVERSATION) == 0) {
        return n;
    }
    n.talks = true;
    n.fingerprint = printOf(plugin);
    n.id = QString::fromUtf8(info.id);
    n.name = QString::fromUtf8(info.name);
    n.version = QString::fromUtf8(info.version);
    n.consentPath = storePath("consent.json");
    n.securityPath = storePath("security.json");
    n.label = vehicleLabel.trimmed().toUtf8();
    n.ecus = ecusOf(plugin);
    ecuio_plugin_publisher_t pub{};
    pub.struct_size = sizeof pub;
    const bool named = ecuio_plugin_publisher(plugin, &pub) == ECUIO_OK;
    if (named) {
        n.publisher = QString::fromUtf8(pub.name);
        n.termsSha = QByteArray(reinterpret_cast<const char*>(pub.terms_sha256),
                                sizeof pub.terms_sha256);
        n.screen = readScreen(plugin);
    }
    // A car is on these; core judges SocketCAN's link, the mock and a
    // replay bind nothing
    const bool car = uri.startsWith(QLatin1String("elm327://")) ||
                     uri.startsWith(QLatin1String("j2534://"));
    n.asks = car && named && !n.screen.isEmpty();
    n.declined = m_declined.contains(n.fingerprint);
    return n;
}

ConsentFound ConsentFlow::applyFound(ecuio_session_t* session,
                                     const ConsentNeed& need)
{
    ConsentFound f;
    if (!need.talks) {
        return f;
    }
    if (ecuio_session_security_store(
            session, need.securityPath.toUtf8().constData(),
            need.label.isEmpty() ? nullptr : need.label.constData()) !=
        ECUIO_OK) {
        f.lines << QStringLiteral("Unlock counts kept for this session only: "
                                  "the security store doesn't read: %1")
                       .arg(detail());
    }
    if (!need.asks) {
        return f;
    }
    if (need.declined) {
        f.lines << QStringLiteral("Consent declined earlier: %1 reads "
                                  "signals only")
                       .arg(need.name);
        return f;
    }
    ecuio_consent_store_t* store = nullptr;
    if (ecuio_consent_store_open(need.consentPath.toUtf8().constData(),
                                 &store) != ECUIO_OK) {
        f.lines << QStringLiteral("The consent store doesn't read: %1. %2 "
                                  "reads signals only")
                       .arg(detail(), need.name);
        return f;
    }
    const auto* print =
        reinterpret_cast<const uint8_t*>(need.fingerprint.constData());
    ecuio_consent_record_t r{};
    r.struct_size = sizeof r;
    if (ecuio_consent_store_find(store, print, &r) == ECUIO_OK) {
        f.lines << (ecuio_session_apply_consent(session, store, print) ==
                            ECUIO_OK
                        ? QStringLiteral("Consent applied: %1 %2")
                              .arg(need.name, need.version)
                        : QStringLiteral("Consent not applied: %1").arg(detail()));
    } else if (ecuio_consent_store_latest(store, need.id.toUtf8().constData(),
                                          &r) == ECUIO_OK) {
        f.waits = true;
        f.changed = recordMap(r);
        f.changed.insert(QStringLiteral("publisherSame"),
                         need.publisher == QString::fromUtf8(r.publisher));
        f.changed.insert(QStringLiteral("termsSame"),
                         need.termsSha == QByteArray(reinterpret_cast<const char*>(
                                                         r.terms_sha256),
                                                     32));
    } else {
        f.waits = true;
    }
    ecuio_consent_store_close(store);
    return f;
}

void ConsentFlow::wait(ecuio_session_t* session, const ConsentNeed& need,
                       const ConsentFound& found, std::function<void()> done)
{
    m_waitSession = session;
    m_waitNeed = need;
    m_waitDone = std::move(done);
    m_answering = false;
    m_ask = need.screen;
    m_ask.insert(QStringLiteral("changed"), found.changed);
    m_ask.insert(QStringLiteral("limitMs"), m_limitMs);
    m_limit->start(m_limitMs);
    log(QStringLiteral("Consent asked: %1 %2").arg(need.name, need.version));
    emit askChanged();
}

void ConsentFlow::answer(bool allow)
{
    if (m_waitSession == nullptr || m_answering) {
        return;
    }
    if (!allow) {
        decline(QStringLiteral("Consent declined: %1 reads signals only "
                               "until Havne quits")
                    .arg(m_waitNeed.name));
        return;
    }
    m_limit->stop();
    m_answering = true;
    m_ask.clear();
    emit askChanged();
    const ConsentNeed need = m_waitNeed;
    ecuio_session_t* session = m_waitSession;
    post([this, need, session]() {
        const Given g = giveAndApply(PluginStore::pathOf(need.sha),
                                     need.consentPath, need.fingerprint,
                                     session);
        QMetaObject::invokeMethod(
            this,
            [this, g, need]() {
                jobDone();
                finishWait(
                    g.ok ? QStringLiteral("Consent given: %1 %2, "
                                          "fingerprint %3")
                               .arg(need.name, need.version,
                                    shortPrint(need.fingerprint))
                    : g.given
                        ? QStringLiteral("Consent given but not applied: %1. "
                                         "%2 reads signals only this session")
                              .arg(g.words, need.name)
                        : QStringLiteral("Consent not given: %1. %2 reads "
                                         "signals only")
                              .arg(g.words, need.name));
                refresh();
            },
            Qt::QueuedConnection);
    });
}

void ConsentFlow::decline(const QString& line)
{
    if (m_waitSession == nullptr || m_answering) {
        return;
    }
    m_limit->stop();
    m_declined.insert(m_waitNeed.fingerprint);
    finishWait(line);
}

void ConsentFlow::finishWait(const QString& line)
{
    log(line);
    m_waitSession = nullptr;
    m_answering = false;
    m_ask.clear();
    emit askChanged();
    if (auto done = std::exchange(m_waitDone, {})) {
        done();
    }
}

QVariantMap ConsentFlow::screenFor(const QString& sha)
{
    for (const QVariant& v : m_store->all()) {
        const QVariantMap e = v.toMap();
        const QStringList profiles = e.value(QStringLiteral("profiles")).toStringList();
        if (e.value(QStringLiteral("sha256")).toString() != sha ||
            profiles.isEmpty()) {
            continue;
        }
        QString found;
        const ecuio_plugin_t* plugin =
            m_store->pluginOfProfile(profiles.first(), &found);
        return plugin != nullptr && e.value(QStringLiteral("conversation")).toBool()
                   ? readScreen(plugin)
                   : QVariantMap{};
    }
    return {};
}

void ConsentFlow::give(const QString& sha)
{
    const QString path = PluginStore::pathOf(sha);
    const QString storeAt = storePath("consent.json");
    post([this, path, storeAt]() {
        const Given g = giveAndApply(path, storeAt, {}, nullptr);
        QMetaObject::invokeMethod(
            this,
            [this, g]() {
                jobDone();
                if (g.ok) {
                    m_declined.remove(g.print);
                }
                const QString words =
                    g.ok ? QStringLiteral("Consent given, fingerprint %1")
                               .arg(shortPrint(g.print))
                         : QStringLiteral("Consent not given: %1").arg(g.words);
                log(words);
                emit given(g.ok, words);
                refresh();
            },
            Qt::QueuedConnection);
    });
}

void ConsentFlow::refresh()
{
    const QString storeAt = storePath("consent.json");
    post([this, storeAt]() {
        ecuio_consent_store_t* store = nullptr;
        QVariantList list;
        QString words;
        if (ecuio_consent_store_open(storeAt.toUtf8().constData(), &store) !=
            ECUIO_OK) {
            words = detail();
        } else {
            uint32_t count = 0;
            (void)ecuio_consent_store_count(store, &count);
            for (uint32_t i = 0; i < count; ++i) {
                ecuio_consent_record_t r{};
                r.struct_size = sizeof r;
                if (ecuio_consent_store_get(store, i, &r) == ECUIO_OK) {
                    list << recordMap(r);
                }
            }
            ecuio_consent_store_close(store);
        }
        QMetaObject::invokeMethod(
            this,
            [this, list, words]() {
                jobDone();
                if (!words.isEmpty()) {
                    log(QStringLiteral("The consent store doesn't read: %1")
                            .arg(words));
                    return;
                }
                if (list != m_records) {
                    m_records = list;
                    emit recordsChanged();
                }
            },
            Qt::QueuedConnection);
    });
}

void ConsentFlow::withdraw(const QString& id, const QString& fingerprint)
{
    const QByteArray print = QByteArray::fromHex(fingerprint.toLatin1());
    if (!fingerprint.isEmpty() && print.size() != 32) {
        emit withdrawn(false, QStringLiteral("Not a fingerprint"));
        return;
    }
    const QString storeAt = storePath("consent.json");
    post([this, id, print, storeAt]() {
        ecuio_consent_store_t* store = nullptr;
        ecuio_status_t rc =
            ecuio_consent_store_open(storeAt.toUtf8().constData(), &store);
        if (rc == ECUIO_OK) {
            rc = ecuio_consent_store_withdraw(
                store, id.toUtf8().constData(),
                print.isEmpty()
                    ? nullptr
                    : reinterpret_cast<const uint8_t*>(print.constData()));
        }
        const QString words =
            rc == ECUIO_OK ? QStringLiteral("Consent withdrawn: %1").arg(id)
                           : QStringLiteral("Consent not withdrawn: %1")
                                 .arg(detail());
        if (store != nullptr) {
            ecuio_consent_store_close(store);
        }
        QMetaObject::invokeMethod(
            this,
            [this, rc, words]() {
                jobDone();
                log(words);
                emit withdrawn(rc == ECUIO_OK, words);
                refresh();
            },
            Qt::QueuedConnection);
    });
}

void ConsentFlow::begin(ecuio_session_t* session, const ConsentNeed& need)
{
    m_session = session;
    m_ecus = need.talks ? need.ecus : QVariantList{};
    m_vehicleNamed = !need.label.isEmpty();
    for (uint32_t c = 0; c < ECUIO_MAX_CHANNELS; ++c) {
        m_generations[c] = 0;
        m_asking[c] = false;
    }
    if (!m_conversations.isEmpty()) {
        m_conversations.clear();
        emit conversationsChanged();
    }
}

void ConsentFlow::poll(uint32_t channels)
{
    if (m_session == nullptr || m_ecus.isEmpty()) {
        return;
    }
    bool moved = false;
    ecuio_channel_conversation_t r[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t c = 0; c < channels && c < ECUIO_MAX_CHANNELS; ++c) {
        if (ecuio_session_get_channel_conversation(m_session, c, &r[c]) !=
            ECUIO_OK) {
            r[c] = ecuio_channel_conversation_t{};
        }
        moved = moved || r[c].generation != m_generations[c];
        m_generations[c] = r[c].generation;
    }
    if (!moved) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVariantList out;
    for (uint32_t c = 0; c < channels && c < ECUIO_MAX_CHANNELS; ++c) {
        if (r[c].state == ECUIO_CONVERSATION_NONE) {
            continue;
        }
        QVariantMap e;
        for (const QVariant& v : std::as_const(m_ecus)) {
            if (v.toMap().value(QStringLiteral("rx")).toLongLong() == r[c].rx_id) {
                e = v.toMap();
            }
        }
        e.insert(QStringLiteral("channel"), static_cast<int>(c));
        e.insert(QStringLiteral("rx"), qint64{r[c].rx_id});
        e.insert(QStringLiteral("state"), wordOf(kStates, std::size(kStates), r[c].state));
        e.insert(QStringLiteral("unlock"), wordOf(kUnlocks, std::size(kUnlocks), r[c].unlock));
        e.insert(QStringLiteral("level"), int{r[c].level});
        e.insert(QStringLiteral("nrc"), int{r[c].nrc});
        e.insert(QStringLiteral("failed"), qint64{r[c].failed});
        e.insert(QStringLiteral("delayLeftMs"), qint64{r[c].delay_left_ms});
        e.insert(QStringLiteral("delayEndsMs"), now + r[c].delay_left_ms);
        e.insert(QStringLiteral("kept"),
                 (r[c].flags & ECUIO_CONVERSATION_NOT_KEPT) == 0);
        out << e;
    }
    m_conversations = out;
    emit conversationsChanged();
}

void ConsentFlow::end()
{
    m_session = nullptr;
    m_ecus.clear();
    if (!m_conversations.isEmpty()) {
        m_conversations.clear();
        emit conversationsChanged();
    }
}

QVariantList ConsentFlow::unlockLines() const
{
    QVariantList out;
    ecuio_consent_line_t l{};
    l.struct_size = sizeof l;
    for (uint32_t i = 0;
         ecuio_consent_line(ECUIO_CONSENT_SET_CONVERSATION, i, &l) == ECUIO_OK;
         ++i) {
        if (l.id == ECUIO_CONSENT_LINE_ENGINE_STATE ||
            l.id == ECUIO_CONSENT_LINE_LOCKOUT) {
            out << QVariantMap{
                {QStringLiteral("key"), QString::fromUtf8(l.key)},
                {QStringLiteral("heading"), QString::fromUtf8(l.heading)},
                {QStringLiteral("text"), QString::fromUtf8(l.text)}};
        }
    }
    return out;
}

namespace {

// What the user is told of an unlock core took or refused
QString unlockWords(ecuio_status_t rc, uint32_t waitMs, const QString& why,
                    bool vehicleNamed)
{
    switch (rc) {
    case ECUIO_OK:
        return QStringLiteral("Unlock asked");
    case ECUIO_ERR_TOO_SOON:
        return waitMs > 0
                   ? QStringLiteral("Unlock refused: the module's delay runs, "
                                    "%1 s left")
                         .arg((waitMs + 999) / 1000)
                   : QStringLiteral("Unlock not asked: the module's identity "
                                    "is still being read. Try again");
    case ECUIO_ERR_REPORT_CHANGED:
        return QStringLiteral("Unlock not asked: the module's state changed. "
                              "Look, then try again");
    case ECUIO_ERR_INVALID_ARGUMENT:
        // Core's refusal for a module with no identity speaks the CLI's
        // vocabulary (a label); the app's label is the picked vehicle's
        if (!vehicleNamed && why.contains(QLatin1String("no label was given"))) {
            return QStringLiteral(
                "Unlock refused: nothing names this car. Pick or add the "
                "vehicle in Settings, Vehicle, or let identification read "
                "its VIN, then connect again");
        }
        [[fallthrough]];
    default:
        return QStringLiteral("Unlock refused: %1").arg(why);
    }
}

} // namespace

QVariantMap ConsentFlow::unlock(int channel, int level)
{
    const auto refused = [](const QString& words) {
        return QVariantMap{{QStringLiteral("ok"), false},
                           {QStringLiteral("words"), words}};
    };
    ecuio_channel_conversation_t r{};
    if (m_session == nullptr || channel < 0 || channel >= ECUIO_MAX_CHANNELS ||
        level <= 0 || level > 0xFF ||
        ecuio_session_get_channel_conversation(
            m_session, static_cast<uint32_t>(channel), &r) != ECUIO_OK) {
        return refused(QStringLiteral("No such module"));
    }
    if (r.state != ECUIO_CONVERSATION_TALKING) {
        return refused(QStringLiteral("The plugin isn't talking to this module"));
    }
    if (m_asking[channel]) {
        return refused(QStringLiteral("An unlock is under way"));
    }
    m_asking[channel] = true;
    ecuio_session_t* session = m_session;
    // The report the row showed: core refuses the word if it moved since
    const uint32_t generation = m_generations[channel];
    const bool named = m_vehicleNamed;
    post([this, session, channel, level, generation, named]() {
        m_unlockThread.store(QThread::currentThreadId());
        uint32_t waitMs = 0;
        const ecuio_status_t rc = ecuio_session_security_unlock(
            session, static_cast<uint32_t>(channel),
            static_cast<uint8_t>(level), generation, &waitMs);
        const QString words = unlockWords(rc, waitMs, detail(), named);
        QMetaObject::invokeMethod(
            this,
            [this, rc, channel, level, words]() {
                jobDone();
                m_asking[channel] = false;
                log(QStringLiteral("%1 (module %2, level %3)")
                        .arg(words)
                        .arg(channel)
                        .arg(level, 2, 16, QLatin1Char('0')));
                emit unlockAnswered(channel, rc == ECUIO_OK, words);
            },
            Qt::QueuedConnection);
    });
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QStringLiteral("Asking")}};
}

void ConsentFlow::log(const QString& line)
{
    if (m_log) {
        m_log(line);
    }
}

void ConsentFlow::post(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop) {
            return;
        }
        m_jobs.push_back(std::move(job));
    }
    if (!m_worker.joinable()) {
        m_worker = std::thread([this]() { work(); });
    }
    m_cv.notify_all();
    if (m_jobsOut++ == 0) {
        emit busyChanged();
    }
}

void ConsentFlow::jobDone()
{
    if (m_jobsOut > 0 && --m_jobsOut == 0) {
        emit busyChanged();
    }
}

void ConsentFlow::work()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_cv.wait(lock, [this]() { return m_stop || !m_jobs.empty(); });
        if (m_jobs.empty()) {
            return; // stopped, nothing left
        }
        std::function<void()> job = std::move(m_jobs.front());
        m_jobs.pop_front();
        m_running = true;
        lock.unlock();
        job();
        lock.lock();
        m_running = false;
        m_cv.notify_all();
    }
}

void ConsentFlow::drain()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this]() { return m_jobs.empty() && !m_running; });
}

void ConsentFlow::join()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}
