// File/layer: app/readonce.cpp | Qt6 Core, app layer, C++17
// Testing: UNIT: ReadOnceTest.* (test_bridge); HARNESS:
//          BridgeTest.ReadOnce* against the fake J2534 DLL's ECU

#include "readonce.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

#include "ecuio/core.h"

namespace {

QString hexPairs(const uint8_t* data, uint16_t length)
{
    QStringList pairs;
    pairs.reserve(length);
    for (uint16_t i = 0; i < length; ++i) {
        pairs << QStringLiteral("%1")
                     .arg(static_cast<uint>(data[i]), 2, 16, QLatin1Char('0'))
                     .toUpper();
    }
    return pairs.join(QLatin1Char(' '));
}

// The request's bytes: 01 0C, 22 1E 1C
QString requestText(const ReadOnce::Request& r)
{
    uint8_t bytes[3] = {r.service, 0, 0};
    if (r.service == 0x22) {
        bytes[1] = static_cast<uint8_t>(r.number >> 8);
        bytes[2] = static_cast<uint8_t>(r.number & 0xFF);
        return hexPairs(bytes, 3);
    }
    bytes[1] = static_cast<uint8_t>(r.number);
    return hexPairs(bytes, 2);
}

QString hexId(uint32_t id)
{
    return QString::number(id, 16).toUpper();
}

// The loader's words about the draft, as the form's: no line and column
// of a file the user never sees, and the module and signal by what they are
QString draftWords(QString words)
{
    words.remove(
        QRegularExpression(QStringLiteral("^line \\d+, column \\d+: ")));
    words.replace(QStringLiteral("signal 'read_once'"),
                  QStringLiteral("the signal"));
    words.replace(QStringLiteral("ecu 'module'"), QStringLiteral("the module"));
    if (!words.isEmpty()) {
        words[0] = words[0].toUpper();
    }
    return words;
}

// A hex ID of 1 to 8 digits, no prefix, at most 29 bits
bool parseId(const QVariant& v, uint32_t* out)
{
    const QString s = v.toString();
    if (s.isEmpty() || s.size() > 8) {
        return false;
    }
    bool ok = false;
    const uint id = s.toUInt(&ok, 16);
    if (!ok || id == 0 || id > 0x1FFFFFFFu) {
        return false;
    }
    *out = id;
    return true;
}

// The keys that say what a session asks for, which Read once sets itself
bool isRequestKey(const QString& key)
{
    static const QStringList keys = {
        QStringLiteral("profile"),  QStringLiteral("did"),
        QStringLiteral("poll"),     QStringLiteral("filter"),
        QStringLiteral("txid"),     QStringLiteral("probe"),
        QStringLiteral("identify"), QStringLiteral("diagnose"),
        QStringLiteral("pack"),     QStringLiteral("scan"),
    };
    return keys.contains(key);
}


QString hexNumber(uint32_t n, int digits)
{
    return QStringLiteral("%1").arg(n, digits, 16, QLatin1Char('0')).toUpper();
}

// uds-generic's request ID for a reply ID (ADR-0020): rx - 8 for an
// 11-bit one, the swapped pair for 18DA<a><b>; 0 for any other
uint32_t udsGenericTx(uint32_t rx)
{
    if (rx >= 9 && rx <= 0x7FF) {
        return rx - 8;
    }
    if ((rx & 0xFFFF0000u) == 0x18DA0000u) {
        return 0x18DA0000u | ((rx & 0xFFu) << 8) | ((rx >> 8) & 0xFFu);
    }
    return 0;
}

} // namespace

uint32_t ReadOnce::standardTx(uint32_t rx)
{
    if (rx >= 0x7E8 && rx <= 0x7EF) {
        return rx - 8;
    }
    if ((rx & 0xFFFFFF00u) == 0x18DAF100u) {
        return 0x18DA00F1u | ((rx & 0xFFu) << 8);
    }
    return 0;
}

bool ReadOnce::requestFrom(const QVariantMap& signal, const QVariantMap& module,
                           Request* out, QString* why)
{
    Request r;
    const auto refuse = [why](const QString& words) {
        *why = words;
        return false;
    };

    if (!parseId(module.value(QStringLiteral("rx")), &r.rx)) {
        return refuse(QStringLiteral(
            "The module needs its reply ID in hex (7E8, 18DAF110)"));
    }
    if (module.contains(QStringLiteral("tx"))) {
        if (!parseId(module.value(QStringLiteral("tx")), &r.tx)) {
            return refuse(QStringLiteral(
                "The module's request ID needs to be hex (7E0)"));
        }
    } else {
        r.tx = standardTx(r.rx);
        if (r.tx == 0) {
            return refuse(
                QStringLiteral("The module needs its request ID: only 7E8 to "
                               "7EF and 18DAF1xx have one by rule"));
        }
    }
    if ((r.rx > 0x7FF) != (r.tx > 0x7FF)) {
        return refuse(
            QStringLiteral("The module's two IDs need the same width"));
    }

    const QVariantMap read = signal.value(QStringLiteral("read")).toMap();
    const QString service = read.value(QStringLiteral("service")).toString();
    const QString number = read.value(QStringLiteral("number")).toString();
    bool ok = false;
    const uint n = number.toUInt(&ok, 16);
    if (service == QStringLiteral("22")) {
        if (number.size() != 4 || !ok) {
            return refuse(QStringLiteral("A DID is four hex digits (D110)"));
        }
        r.service = 0x22;
    } else if (service == QStringLiteral("01")) {
        if (number.size() != 2 || !ok) {
            return refuse(QStringLiteral("A PID is two hex digits (0C)"));
        }
        r.service = 0x01;
    } else {
        return refuse(
            QStringLiteral("The service is 01 (a PID) or 22 (a DID)"));
    }
    r.number = static_cast<uint16_t>(n);

    const QString kind =
        signal.value(QStringLiteral("kind"), QStringLiteral("number"))
            .toString();
    if (signal.value(QStringLiteral("name")).toString().trimmed().isEmpty()) {
        return refuse(QStringLiteral("A signal needs a name"));
    }
    if (kind == QStringLiteral("computed")) {
        return refuse(
            QStringLiteral("Read once reads a number, text, hex or enum"));
    }

    // The draft as a one-signal file, which core's loader reads as it reads
    // any: its rules and its words are the only check, and the def it
    // makes is the one a session would decode with
    QVariantMap draft = signal;
    draft.insert(QStringLiteral("key"), QStringLiteral("read_once"));
    draft.insert(QStringLiteral("ecu"), QStringLiteral("module"));
    const QVariantMap ecu{{QStringLiteral("id"), QStringLiteral("module")},
                          {QStringLiteral("rx"), hexId(r.rx)},
                          {QStringLiteral("tx"), hexId(r.tx)}};
    const QByteArray bytes =
        QByteArrayLiteral("{\"havne_signals\":1,\"name\":\"Read once\","
                          "\"source\":{\"name\":\"Read once\"},\"ecus\":[") +
        QJsonDocument(QJsonObject::fromVariantMap(ecu))
            .toJson(QJsonDocument::Compact) +
        QByteArrayLiteral("],\"signals\":[") +
        QJsonDocument(QJsonObject::fromVariantMap(draft))
            .toJson(QJsonDocument::Compact) +
        QByteArrayLiteral("]}");
    ecuio_signal_file_t* file = nullptr;
    if (ecuio_signal_file_load(bytes.constData(),
                               static_cast<size_t>(bytes.size()),
                               &file) != ECUIO_OK) {
        return refuse(draftWords(QString::fromUtf8(ecuio_last_error_detail())));
    }
    const ecuio_signal_def_t* def = ecuio_signal_file_find(file, "read_once");
    if (def == nullptr) {
        ecuio_signal_file_free(file);
        return refuse(QStringLiteral("Core made no signal of the draft"));
    }
    r.def = *def;
    ecuio_signal_file_free(file);
    r.labels = signal.value(QStringLiteral("values")).toMap();
    *out = r;
    return true;
}

QString ReadOnce::uriFor(const QString& adapterUri, const Request& r,
                         QString* why)
{
    const QString uri = adapterUri.trimmed();
    const QString lower = uri.toLower();
    if (!lower.startsWith(QStringLiteral("j2534://")) &&
        !lower.startsWith(QStringLiteral("elm327://")) &&
        !lower.startsWith(QStringLiteral("socketcan://"))) {
        *why = QStringLiteral(
            "Read once needs an adapter: j2534://, elm327:// or socketcan://");
        return {};
    }
    const qsizetype q = uri.indexOf(QLatin1Char('?'));
    const QString base = q < 0 ? uri : uri.left(q);
    QStringList keys;
    if (q >= 0) {
        for (const QString& kv :
             uri.mid(q + 1).split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
            if (!isRequestKey(kv.section(QLatin1Char('='), 0, 0))) {
                keys << kv;
            }
        }
    }
    if (r.service == 0x22) {
        // uds-generic addresses a module by its reply ID alone, its
        // request ID found by ADR-0020's rule
        if (udsGenericTx(r.rx) != r.tx) {
            *why = QStringLiteral("Read once asks for a DID only where the "
                                  "request ID follows the reply ID's "
                                  "rule (rx - 8, or the swapped 29-bit pair)");
            return {};
        }
        keys << QStringLiteral("profile=uds-generic")
             << QStringLiteral("did=%1:%2:hex")
                    .arg(hexId(r.rx), hexNumber(r.number, 4))
             << QStringLiteral("identify=0") << QStringLiteral("diagnose=0")
             << QStringLiteral("probe=0");
    } else {
        // ?poll= asks the one PID, nothing else (no rotation, no
        // identification). On SocketCAN a profile with no channels only
        // listens and refuses the request keys, so the PID goes through
        // obd2's, which requests; elsewhere raw takes them as they are
        const bool socketcan = lower.startsWith(QStringLiteral("socketcan://"));
        keys << (socketcan ? QStringLiteral("profile=obd2")
                           : QStringLiteral("profile=raw"))
             << QStringLiteral("poll=0x%1").arg(hexNumber(r.number, 2))
             << QStringLiteral("filter=0x%1").arg(hexId(r.rx))
             << QStringLiteral("txid=0x%1").arg(hexId(r.tx))
             << QStringLiteral("probe=0");
    }
    return base + QLatin1Char('?') + keys.join(QLatin1Char('&'));
}

bool ReadOnce::answers(const Request& r, uint32_t rx, const uint8_t* data,
                       uint16_t length)
{
    if (rx != r.rx || length < 2) {
        return false;
    }
    if (data[0] == 0x7F) {
        return length >= 3 && data[1] == r.service && data[2] != 0x78;
    }
    if (r.service == 0x22) {
        return length >= 3 && data[0] == 0x62 && data[1] == (r.number >> 8) &&
               data[2] == (r.number & 0xFF);
    }
    return data[0] == 0x41 && data[1] == r.number;
}

QString ReadOnce::nrcName(uint8_t nrc)
{
    switch (nrc) {
    case 0x10:
        return QStringLiteral("generalReject");
    case 0x11:
        return QStringLiteral("serviceNotSupported");
    case 0x12:
        return QStringLiteral("subFunctionNotSupported");
    case 0x13:
        return QStringLiteral("incorrectMessageLengthOrInvalidFormat");
    case 0x14:
        return QStringLiteral("responseTooLong");
    case 0x21:
        return QStringLiteral("busyRepeatRequest");
    case 0x22:
        return QStringLiteral("conditionsNotCorrect");
    case 0x24:
        return QStringLiteral("requestSequenceError");
    case 0x25:
        return QStringLiteral("noResponseFromSubnetComponent");
    case 0x26:
        return QStringLiteral("failurePreventsExecutionOfRequestedAction");
    case 0x31:
        return QStringLiteral("requestOutOfRange");
    case 0x33:
        return QStringLiteral("securityAccessDenied");
    case 0x35:
        return QStringLiteral("invalidKey");
    case 0x36:
        return QStringLiteral("exceededNumberOfAttempts");
    case 0x37:
        return QStringLiteral("requiredTimeDelayNotExpired");
    case 0x78:
        return QStringLiteral("requestCorrectlyReceivedResponsePending");
    case 0x7E:
        return QStringLiteral("subFunctionNotSupportedInActiveSession");
    case 0x7F:
        return QStringLiteral("serviceNotSupportedInActiveSession");
    default:
        return {};
    }
}

QVariantMap ReadOnce::noReply(const Request& r, const QString& words)
{
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("request"), requestText(r)},
            {QStringLiteral("reply"), QString()},
            {QStringLiteral("value"), QString()},
            {QStringLiteral("nrc"), 0},
            {QStringLiteral("words"), words}};
}

QVariantMap ReadOnce::evaluate(const Request& r, const uint8_t* data,
                               uint16_t length)
{
    QVariantMap out = noReply(r, QString());
    out.insert(QStringLiteral("reply"), hexPairs(data, length));
    if (length >= 3 && data[0] == 0x7F) {
        const QString name = nrcName(data[2]);
        out.insert(QStringLiteral("nrc"), data[2]);
        out.insert(
            QStringLiteral("words"),
            name.isEmpty()
                ? QStringLiteral("Refused: 7F %1").arg(hexPairs(data + 1, 2))
                : QStringLiteral("Refused: %1 (7F %2)")
                      .arg(name, hexPairs(data + 1, 2)));
        return out;
    }

    const ecuio_signal_def_t& d = r.def;
    if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
        char text[ECUIO_DECODE_TEXT_MAX] = {};
        const int rc = ecuio_decode_text(&d, data, length, text, sizeof(text));
        if (rc != ECUIO_DECODE_OK) {
            out.insert(
                QStringLiteral("words"),
                rc == ECUIO_DECODE_ERR_PAST_PAYLOAD
                    ? QStringLiteral(
                          "The field runs past the reply's %1 data bytes")
                          .arg(length - d.mux_length)
                    : QStringLiteral("Core couldn't decode it (code %1)")
                          .arg(rc));
            return out;
        }
        out.insert(QStringLiteral("ok"), true);
        out.insert(QStringLiteral("value"), QString::fromUtf8(text));
        return out;
    }

    double v = 0.0;
    const int rc = ecuio_decode_signal(&d, data, length, &v);
    if (rc != ECUIO_DECODE_OK) {
        out.insert(
            QStringLiteral("words"),
            rc == ECUIO_DECODE_ERR_PAST_PAYLOAD
                ? QStringLiteral(
                      "The field runs past the reply's %1 data bytes")
                      .arg(length - d.mux_length)
                : QStringLiteral("Core couldn't decode it (code %1)").arg(rc));
        return out;
    }
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("number"), v);
    if (d.kind == ECUIO_VALUE_ENUM) {
        // A raw value with no label shows as its number, as Live will
        const QString raw = QString::number(static_cast<qulonglong>(v));
        const QString label = r.labels.value(raw).toString();
        out.insert(QStringLiteral("value"),
                   label.isEmpty() ? raw
                                   : QStringLiteral("%1 (%2)").arg(label, raw));
        return out;
    }
    const QString unit = QString::fromUtf8(d.unit);
    const QString number = QString::number(v, 'f', d.decimal_places);
    out.insert(QStringLiteral("value"),
               unit.isEmpty() ? number : number + QLatin1Char(' ') + unit);
    return out;
}

ReadOnce::~ReadOnce()
{
    join();
}

void ReadOnce::join()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_busy.store(false, std::memory_order_release);
}

namespace {

// The first frame that answers, copied off the worker
struct Capture {
    ReadOnce::Request request;
    std::mutex mutex;
    std::condition_variable cv;
    bool got = false;
    std::vector<uint8_t> data;
};

void onFrame(const ecuio_frame_t* frame, void* user)
{
    auto* c = static_cast<Capture*>(user);
    if (!ReadOnce::answers(c->request, frame->arbitration_id, frame->data,
                           frame->data_length)) {
        return;
    }
    std::lock_guard<std::mutex> lock(c->mutex);
    if (!c->got) {
        c->data.assign(frame->data, frame->data + frame->data_length);
        c->got = true;
        c->cv.notify_all();
    }
}

QString failure(const char* what, ecuio_status_t rc)
{
    QString words = QStringLiteral("%1: %2").arg(
        QString::fromUtf8(what), QString::fromUtf8(ecuio_status_string(rc)));
    const char* detail = ecuio_last_error_detail();
    if (detail != nullptr && detail[0] != '\0') {
        words += QStringLiteral(": ") + QString::fromUtf8(detail);
    }
    return words;
}

} // namespace

bool ReadOnce::start(const QString& uri, const Request& r, int waitMs,
                     std::function<void(const QVariantMap&)> done)
{
    bool expected = false;
    if (!m_busy.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
        return false;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_thread = std::thread([uri, r, waitMs, done = std::move(done)]() {
        QVariantMap result;
        ecuio_session_t* session = nullptr;
        const QByteArray u = uri.toUtf8();
        ecuio_status_t rc = ecuio_session_create(u.constData(), &session);
        if (rc != ECUIO_OK) {
            done(noReply(r, failure("Couldn't open the adapter", rc)));
            return;
        }
        Capture capture;
        capture.request = r;
        ecuio_register_frame_callback(session, onFrame, &capture);
        rc = ecuio_session_start(session);
        if (rc != ECUIO_OK) {
            result = noReply(r, failure("Couldn't start the session", rc));
        } else {
            std::unique_lock<std::mutex> lock(capture.mutex);
            capture.cv.wait_for(lock, std::chrono::milliseconds(waitMs),
                                [&capture]() { return capture.got; });
        }
        // Closed before the reply is read: close joins the worker, so
        // no callback touches the capture after it returns
        ecuio_session_close(session);
        ecuio_session_destroy(session);
        if (rc == ECUIO_OK) {
            result = capture.got
                         ? evaluate(r, capture.data.data(),
                                    static_cast<uint16_t>(capture.data.size()))
                         : noReply(r, QStringLiteral("No reply in %1 s")
                                          .arg(QString::number(waitMs / 1000.0,
                                                               'g', 3)));
        }
        done(result);
    });
    return true;
}
