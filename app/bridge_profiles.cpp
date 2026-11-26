// File/layer: app/bridge_profiles.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "ecuio/j2534.h"
#include "ecuio/serial.h"
#include "ecuio/signal_sets.h"
#include "ecuio/socketcan.h"
#include "signalmodules.h"

#include <QMetaObject>
#include <QByteArray>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>

QString SessionBridge::readOnce(const QString& adapterUri,
                                const QVariantMap& signal,
                                const QVariantMap& module)
{
    // The adapter is the session's while one runs, and a second open
    // of it may be refused: Read once waits for none
    if (m_connected.load(std::memory_order_acquire) || m_connecting ||
        m_disconnecting) {
        return QStringLiteral(
            "Read once reads only while no session is connected");
    }
    if (m_readOnce.busy()) {
        return QStringLiteral("A read is running");
    }
    ReadOnce::Request request;
    QString why;
    if (!ReadOnce::requestFrom(signal, module, &request, &why)) {
        return why;
    }
    const QString uri = ReadOnce::uriFor(adapterUri, request, &why);
    if (uri.isEmpty()) {
        return why;
    }
    // An app-fed stream's link is readied here, as a connect's is
    if (m_streamPreparer) {
        why = m_streamPreparer(uri);
        if (!why.isEmpty()) {
            return why;
        }
    }
    applyFrameLoggingEnv();
    const bool started = m_readOnce.start(
        uri, request, m_readOnceWaitMs, [this, uri](const QVariantMap& result) {
            QMetaObject::invokeMethod(
                this,
                [this, uri, result]() {
                    m_readOnce.join();
                    QVariantMap shown = result;
                    shown.insert(QStringLiteral("uri"), uri);
                    emit readingOnceChanged();
                    emit logMessage(
                        QStringLiteral("Read once: %1 %2")
                            .arg(result.value(QStringLiteral("request"))
                                     .toString(),
                                 result.value(QStringLiteral("ok")).toBool()
                                     ? QStringLiteral("answered")
                                     : result.value(QStringLiteral("words"))
                                           .toString()));
                    emit readOnceResult(shown);
                },
                Qt::QueuedConnection);
        });
    if (!started) {
        return QStringLiteral("A read is running");
    }
    emit readingOnceChanged();
    return {};
}

QVariantMap SessionBridge::gaugeSignals() const
{
    const auto ids = [](int mock, int obd2) {
        return QVariantList{QStringLiteral("id:%1").arg(mock),
                            QStringLiteral("id:%1").arg(obd2)};
    };
    return {
        {QStringLiteral("rpm"),
         ids(ECUIO_MOCK_SIG_RPM_ID, ECUIO_OBD2_SIG_RPM_ID)},
        {QStringLiteral("speed"),
         ids(ECUIO_MOCK_SIG_SPEED_ID, ECUIO_OBD2_SIG_SPEED_ID)},
        {QStringLiteral("coolant"),
         ids(ECUIO_MOCK_SIG_COOLANT_ID, ECUIO_OBD2_SIG_COOLANT_TEMP_ID)},
        {QStringLiteral("oil"),
         ids(ECUIO_MOCK_SIG_OIL_ID, ECUIO_OBD2_SIG_OIL_TEMP_ID)},
        {QStringLiteral("iat"),
         ids(ECUIO_MOCK_SIG_IAT_ID, ECUIO_OBD2_SIG_IAT_ID)},
        {QStringLiteral("maf"),
         ids(ECUIO_MOCK_SIG_MAF_ID, ECUIO_OBD2_SIG_MAF_ID)},
        {QStringLiteral("throttle"),
         ids(ECUIO_MOCK_SIG_THROTTLE_ID, ECUIO_OBD2_SIG_THROTTLE_ID)},
        {QStringLiteral("load"),
         ids(ECUIO_MOCK_SIG_LOAD_ID, ECUIO_OBD2_SIG_ENGINE_LOAD_ID)},
        {QStringLiteral("battery"),
         ids(ECUIO_MOCK_SIG_BATTERY_V_ID, ECUIO_OBD2_SIG_BATTERY_V_ID)},
        {QStringLiteral("lambda"),
         ids(ECUIO_MOCK_SIG_LAMBDA_ID, ECUIO_OBD2_PID_SIG_ID(0x44, 0))},
        {QStringLiteral("boost"),
         ids(ECUIO_MOCK_SIG_BOOST_ID, ECUIO_OBD2_SIG_BOOST_ID)},
    };
}

const ecuio_signal_file_t*
SessionBridge::fileOfSet(const ecuio_signal_set_t* set) const
{
    if (set == nullptr) {
        return nullptr;
    }
    const ecuio_signal_file_t* file = m_pluginStore.fileFor(set);
    if (file == nullptr) {
        file = togetherFileOf(set);
    }
    for (auto it = m_replayFiles.begin();
         file == nullptr && it != m_replayFiles.end(); ++it) {
        if (ecuio_signal_file_set(it->second.file) == set) {
            file = it->second.file;
        }
    }
    return file;
}

QByteArray
SessionBridge::signalFileTextOfSet(const ecuio_signal_set_t* set) const
{
    size_t length = 0;
    const char* text = ecuio_signal_file_text(fileOfSet(set), &length);
    return QByteArray(text, static_cast<qsizetype>(length));
}

QString BridgeFormats::profileOfUri(const QString& uri)
{
    // Core's scheme routing and defaults (core.cpp, ecuio_session_create):
    // case-sensitive prefixes; j2534:// on Windows alone, socketcan:// on
    // Linux but not Android (internal.h's ECUIO_HAVE_SOCKETCAN); mock://
    // decodes through mock, an adapter through obd2, a replay and a bus
    // through raw. replayUriFor has its own rule for a recorded URI
    static const struct {
        const char* scheme;
        const char* profile;
    } kSchemes[] = {
        {"mock://", "mock"},   // the mock's own set
        {"replay://", "raw"},  // a recording's frames
        {"elm327://", "obd2"}, // an adapter
#ifdef _WIN32
        {"j2534://", "obd2"}, // an adapter
#endif
#if defined(__linux__) && !defined(__ANDROID__)
        {"socketcan://", "raw"}, // a bus
#endif
    };
    const QByteArray u = uri.toUtf8();
    QString profile;
    for (const auto& s : kSchemes) {
        if (u.startsWith(s.scheme)) {
            profile = QString::fromLatin1(s.profile);
            break;
        }
    }
    if (profile.isEmpty()) {
        return {};
    }
    const qsizetype q = u.indexOf('?');
    if (q < 0) {
        return profile;
    }
    // The query's grammar (core/src/uri_query.h): key=value segments
    // joined by '&', none empty, each with an '='; every profile= value
    // 1 to ECUIO_PROFILE_NAME_MAX bytes, the last one winning. Other keys
    // and the segment count are core's to judge at the connect
    const QList<QByteArray> segments = u.mid(q + 1).split('&');
    for (const QByteArray& segment : segments) {
        const qsizetype eq = segment.indexOf('=');
        if (eq < 0) {
            return {};
        }
        if (segment.left(eq) == "profile") {
            const QByteArray value = segment.mid(eq + 1);
            if (value.isEmpty() || value.size() > ECUIO_PROFILE_NAME_MAX) {
                return {};
            }
            profile = QString::fromUtf8(value);
        }
    }
    return profile;
}

QVariantList SessionBridge::profileSignals(const QString& uri) const
{
    const QString profile = profileOfUri(uri);
    const ecuio_signal_set_t* set = nullptr;
    if (profile.isEmpty() ||
        ecuio_profile_lookup(profile.toUtf8().constData(), &set) != ECUIO_OK ||
        set == nullptr || set->defs == nullptr) {
        return {};
    }
    const ecuio_signal_file_t* file = fileOfSet(set);
    const bool builtIn = SignalListModel::isBuiltIn(set);
    const uint32_t units = m_signalModel->unitSystem();
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(set->count));
    std::vector<ecuio_signal_key_t> keys(set->count);
    uint32_t keyCount = 0;
    (void)ecuio_signal_set_keys(set, file, keys.data(), set->count, &keyCount);
    const std::vector<signal_modules::Row> modules =
        signal_modules::of(set, file, keys);
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        const SignalListModel::DefFacts f =
            SignalListModel::factsOf(file, builtIn, def);
        const ecuio_display_value_t shown =
            displayed(def.unit, def.decimal_places, 0.0, units);
        const std::pair<double, double> range = displayedRange(
            def.unit, def.decimal_places, f.rangeLow, f.rangeHigh, units);
        // The file's own gauge for this def, in its decoded unit (the unit
        // a cell stores); empty when the file gives none
        QVariantMap gauge;
        if (file != nullptr) {
            if (const ecuio_signal_gauge_t* g =
                    ecuio_signal_file_gauge(file, &def)) {
                gauge.insert(QStringLiteral("min"), g->min);
                gauge.insert(QStringLiteral("max"), g->max);
                if ((g->flags & ECUIO_GAUGE_WARN_LOW) != 0) {
                    gauge.insert(QStringLiteral("warnLow"), g->warn_low);
                }
                if ((g->flags & ECUIO_GAUGE_WARN_HIGH) != 0) {
                    gauge.insert(QStringLiteral("warnHigh"), g->warn_high);
                }
            }
        }
        rows.append(QVariantMap{
            {QStringLiteral("id"), static_cast<int>(def.id)},
            {QStringLiteral("ref"), f.ref},
            {QStringLiteral("name"), QString::fromUtf8(def.name)},
            {QStringLiteral("unit"), QString::fromUtf8(shown.unit)},
            {QStringLiteral("decimals"),
             static_cast<int>(shown.decimal_places)},
            {QStringLiteral("isText"),
             def.kind == ECUIO_VALUE_TEXT || def.kind == ECUIO_VALUE_HEX},
            {QStringLiteral("isEnum"), f.isEnum},
            {QStringLiteral("group"), static_cast<int>(def.group)},
            {QStringLiteral("rangeLow"), range.first},
            {QStringLiteral("rangeHigh"), range.second},
            {QStringLiteral("decodedUnit"), QString::fromUtf8(def.unit)},
            {QStringLiteral("decodedLow"), f.rangeLow},
            {QStringLiteral("decodedHigh"), f.rangeHigh},
            {QStringLiteral("gauge"), gauge},
            {QStringLiteral("module"), modules[i].module},
            {QStringLiteral("moduleName"), modules[i].moduleName},
            {QStringLiteral("tag"), modules[i].tag},
        });
    }
    return rows;
}

QVariantList SessionBridge::discoverJ2534Devices() const
{
    QVariantList out;

    ecuio_j2534_device_t* devices = nullptr;
    uint32_t count = 0;
    if (ecuio_j2534_discover_devices(&devices, &count) != ECUIO_OK) {
        return out;   // includes non-Windows NOT_IMPLEMENTED -> empty list
    }
    for (uint32_t i = 0; i < count; ++i) {
        QVariantMap m;
        m["vendorName"] = QString::fromUtf8(devices[i].vendor_name);
        m["dllPath"] = QString::fromUtf8(devices[i].dll_path);
        /* reserved[0] is the loader's PE sniff: 1 = x86, 2 = x64,
         * 0 = unknown. Unknown is displayed honestly: the loader
         * sends it to the x64 surrogate, which is where it would
         * load, not a claim about the binary. */
        m["architecture"] = (devices[i].reserved[0] == 2)
                                ? QStringLiteral("x64")
                            : (devices[i].reserved[0] == 1)
                                ? QStringLiteral("x86")
                                : QStringLiteral("unknown");
        out.append(m);
    }
    ecuio_j2534_free_devices(devices);
    return out;
}

QVariantList SessionBridge::discoverSerialPorts() const
{
    QVariantList out;
    uint32_t count = 0;
    if (ecuio_serial_ports(nullptr, 0, &count) != ECUIO_OK || count == 0) {
        return out;
    }
    /* A port plugged in between the calls is left for the next refresh */
    std::vector<ecuio_serial_port_t> ports(count);
    if (ecuio_serial_ports(ports.data(), count, &count) != ECUIO_OK) {
        return out;
    }
    const uint32_t listed =
        std::min<uint32_t>(count, static_cast<uint32_t>(ports.size()));
    for (uint32_t i = 0; i < listed; ++i) {
        QVariantMap m;
        m["name"] = QString::fromUtf8(ports[i].name);
        m["device"] = QString::fromUtf8(ports[i].device);
        out.append(m);
    }
    return out;
}

namespace {
/* A CAN mode mask in core's words (ip link's), the named bits alone */
QStringList canModeWords(uint32_t modes)
{
    QStringList words;
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        const char* word = (modes & bit) != 0 ? ecuio_can_mode_name(bit) : "";
        if (*word != '\0') {
            words.append(QString::fromLatin1(word));
        }
    }
    return words;
}
} // namespace

QVariantList SessionBridge::discoverCanInterfaces() const
{
    QVariantList out;
    uint32_t count = 0;
    if (ecuio_can_interfaces(nullptr, 0, &count) != ECUIO_OK || count == 0) {
        return out;
    }
    /* An interface added between the calls is left for the next refresh */
    std::vector<ecuio_can_interface_t> list(count);
    if (ecuio_can_interfaces(list.data(), count, &count) != ECUIO_OK) {
        return out;
    }
    const uint32_t listed =
        std::min<uint32_t>(count, static_cast<uint32_t>(list.size()));
    for (uint32_t i = 0; i < listed; ++i) {
        QVariantMap m;
        m["name"] = QString::fromLatin1(list[i].name);
        m["kind"] = QString::fromLatin1(list[i].kind);
        m["up"] = (list[i].flags & ECUIO_CAN_UP) != 0;
        m["listenOnly"] = (list[i].flags & ECUIO_CAN_LISTEN_ONLY) != 0;
        m["fd"] = (list[i].flags & ECUIO_CAN_FD) != 0;
        m["bitrate"] = list[i].bitrate;
        m["state"] = QString::fromLatin1(ecuio_can_state_name(list[i].state));
        m["modes"] = canModeWords(list[i].modes);
        m["modesKnown"] = (list[i].flags & ECUIO_CAN_MODES_KNOWN) != 0;
        m["modesSupported"] = canModeWords(list[i].modes_supported);
        out.append(m);
    }
    return out;
}
