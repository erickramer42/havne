// File/layer: app/readonce.cpp | Qt6 Core, app layer, C++17
// Testing: UNIT: ReadOnceTest.* (test_bridge); HARNESS:
//          BridgeTest.ReadOnce* against the fake J2534 DLL's ECU

#include "readonce.h"

#include <QStringList>

#include <chrono>
#include <cmath>
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

// A whole number from QML (a double) or JSON text, within [lo, hi]
bool wholeIn(const QVariant& v, int lo, int hi, int* out)
{
    bool ok = false;
    const double d = v.toDouble(&ok);
    if (!ok || !(d >= lo && d <= hi) || d != std::floor(d)) {
        return false;
    }
    *out = static_cast<int>(d);
    return true;
}

bool copyText(const QString& s, char* out, size_t cap)
{
    const QByteArray u = s.toUtf8();
    if (static_cast<size_t>(u.size()) >= cap) {
        return false;
    }
    std::memcpy(out, u.constData(), static_cast<size_t>(u.size()));
    out[u.size()] = '\0';
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

QString hexId(uint32_t id)
{
    return QString::number(id, 16).toUpper();
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

    ecuio_signal_def_t& d = r.def;
    d.id = 1;
    d.message_id = r.rx;
    d.mux_offset = 0;
    d.mux_length = r.service == 0x22 ? 3 : 2;
    d.mux_key =
        r.service == 0x22 ? (0x620000u | r.number) : (0x4100u | r.number);
    const int echo = d.mux_length;

    const QString name = signal.value(QStringLiteral("name")).toString();
    if (name.trimmed().isEmpty() || !copyText(name, d.name, sizeof(d.name))) {
        return refuse(QStringLiteral("A name is 1 to 31 bytes"));
    }

    const QString kind =
        signal.value(QStringLiteral("kind"), QStringLiteral("number"))
            .toString();
    const QVariantMap field = signal.value(QStringLiteral("field")).toMap();
    int byte = 0;
    if (!wholeIn(field.value(QStringLiteral("byte")), 0,
                 ECUIO_FRAME_DATA_MAX - 1 - echo, &byte)) {
        return refuse(QStringLiteral(
            "The field needs its first byte, 0 for the first data byte"));
    }
    d.start_byte = static_cast<uint16_t>(echo + byte);

    if (kind == QStringLiteral("text") || kind == QStringLiteral("hex")) {
        int bytes = 0;
        if (field.contains(QStringLiteral("bytes")) &&
            !wholeIn(field.value(QStringLiteral("bytes")), 0,
                     ECUIO_FRAME_DATA_MAX, &bytes)) {
            return refuse(QStringLiteral(
                "A text field's bytes is a count, 0 for the rest"));
        }
        d.kind =
            kind == QStringLiteral("text") ? ECUIO_VALUE_TEXT : ECUIO_VALUE_HEX;
        d.byte_order = ECUIO_BYTE_ORDER_BE;
        d.byte_length = static_cast<uint16_t>(bytes);
        d.scale = 1.0;
    } else if (kind == QStringLiteral("number") ||
               kind == QStringLiteral("enum")) {
        int bits = 0;
        int bit = 0;
        if (!wholeIn(field.value(QStringLiteral("bits")), 1, 32, &bits)) {
            return refuse(
                QStringLiteral("The field's bits is a width from 1 to 32"));
        }
        if (field.contains(QStringLiteral("bit")) &&
            !wholeIn(field.value(QStringLiteral("bit")), 0, 7, &bit)) {
            return refuse(QStringLiteral("The field's bit is 0 to 7"));
        }
        const QString order =
            field.value(QStringLiteral("order"), QStringLiteral("be"))
                .toString();
        if (order != QStringLiteral("be") && order != QStringLiteral("le")) {
            return refuse(QStringLiteral("The field's order is be or le"));
        }
        const bool big = order == QStringLiteral("be");
        if (big && (bits % 8 != 0 || bit != 0)) {
            return refuse(QStringLiteral(
                "A big-endian field is whole bytes: a narrower one is le"));
        }
        const bool isSigned =
            field.value(QStringLiteral("signed"), false).toBool();
        d.byte_order = big ? ECUIO_BYTE_ORDER_BE : ECUIO_BYTE_ORDER_LE;
        d.bit_offset = static_cast<uint8_t>(bit);
        d.bit_length = static_cast<uint8_t>(bits);
        if (kind == QStringLiteral("enum")) {
            if (isSigned) {
                return refuse(QStringLiteral("An enum's field is unsigned"));
            }
            d.kind = ECUIO_VALUE_ENUM;
            d.scale = 1.0;
            r.labels = signal.value(QStringLiteral("values")).toMap();
        } else {
            d.kind = isSigned ? ECUIO_VALUE_INT : ECUIO_VALUE_UINT;
            ok = false;
            d.scale = signal.value(QStringLiteral("scale"), 1.0).toDouble(&ok);
            if (!ok || !std::isfinite(d.scale) || d.scale == 0.0) {
                return refuse(
                    QStringLiteral("The scale is a number other than 0"));
            }
            d.offset =
                signal.value(QStringLiteral("offset"), 0.0).toDouble(&ok);
            if (!ok || !std::isfinite(d.offset)) {
                return refuse(QStringLiteral("The offset is a number"));
            }
            if (!copyText(signal.value(QStringLiteral("unit")).toString(),
                          d.unit, sizeof(d.unit))) {
                return refuse(QStringLiteral("A unit is at most 15 bytes"));
            }
            int decimals = 0;
            if (signal.contains(QStringLiteral("decimals")) &&
                !wholeIn(signal.value(QStringLiteral("decimals")), 0, 3,
                         &decimals)) {
                return refuse(QStringLiteral("Decimals is 0 to 3"));
            }
            d.decimal_places = static_cast<uint8_t>(decimals);
        }
    } else {
        return refuse(
            QStringLiteral("Read once reads a number, text, hex or enum"));
    }

    const int rc = ecuio_validate_signal(&d);
    if (rc != ECUIO_VALIDATE_OK) {
        return refuse(
            QStringLiteral("Core refuses the signal (validator code %1)")
                .arg(rc));
    }
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
        keys << QStringLiteral("profile=raw")
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
