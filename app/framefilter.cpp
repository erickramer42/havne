// File/layer: app/framefilter.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: FrameFilterTest, BridgeTest.FrameFilter* (test_bridge)

#include "framefilter.h"

#include <QRegularExpression>
#include <QStringList>

#include <cmath>

#include "ecuio/core.h"

namespace {

// The largest ID a frame carries: 29 bits
constexpr uint32_t kMaxId = 0x1FFFFFFFu;
// Past this, only an extended frame has the ID
constexpr uint32_t kMaxStandardId = 0x7FFu;

// One hex ID, "0x" optional; false for anything else or past 29 bits
bool hexId(QString text, uint32_t* out)
{
    text = text.trimmed();
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        text = text.mid(2);
    }
    if (text.isEmpty() || text.size() > 8) {
        return false;
    }
    bool ok = false;
    const uint value = text.toUInt(&ok, 16);
    if (!ok || value > kMaxId) {
        return false;
    }
    *out = value;
    return true;
}

// One length in bytes, decimal; false past what a frame holds
bool decimalLength(QString text, int* out)
{
    text = text.trimmed();
    if (text.isEmpty() || text.size() > 4) {
        return false;
    }
    for (const QChar c : text) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
            return false;
        }
    }
    const int value = text.toInt();
    if (value > static_cast<int>(ECUIO_FRAME_DATA_MAX)) {
        return false;
    }
    *out = value;
    return true;
}

// Seconds as the table shows them ("12.5", "30", ".25"), into
// microseconds; false for a sign, an exponent or past a million hours
bool seconds(QString text, uint64_t* out)
{
    static const QRegularExpression form(
        QStringLiteral("^([0-9]+(\\.[0-9]*)?|\\.[0-9]+)$"));
    text = text.trimmed();
    if (!form.match(text).hasMatch()) {
        return false;
    }
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok || value > 3.6e9) {
        return false;
    }
    *out = static_cast<uint64_t>(std::llround(value * 1e6));
    return true;
}

} // namespace

QString FrameFilter::parseIds(const QString& text, std::vector<IdRange>* out)
{
    out->clear();
    if (text.trimmed().isEmpty()) {
        return {};
    }
    std::vector<IdRange> ranges;
    for (const QString& raw : text.split(QLatin1Char(','))) {
        const QString item = raw.trimmed();
        if (item.isEmpty()) {
            return QStringLiteral("an empty item between commas");
        }
        if (item.contains(QLatin1Char('/'))) {
            return QStringLiteral(
                       "\"%1\": an ID and its mask take \":\", as in 7E0:7F8")
                .arg(item);
        }
        IdRange r;
        const qsizetype colon = item.indexOf(QLatin1Char(':'));
        if (colon >= 0) {
            if (!hexId(item.left(colon), &r.first) ||
                !hexId(item.mid(colon + 1), &r.mask)) {
                return QStringLiteral(
                           "\"%1\" isn't a hex ID and mask like 7E0:7F8")
                    .arg(item);
            }
            r.last = r.first;
            r.masked = true;
            ranges.push_back(r);
            continue;
        }
        const qsizetype dash = item.indexOf(QLatin1Char('-'));
        const bool ok =
            dash < 0 ? hexId(item, &r.first)
                     : hexId(item.left(dash), &r.first) &&
                           hexId(item.mid(dash + 1), &r.last);
        if (!ok) {
            return QStringLiteral(
                       "\"%1\" isn't a hex ID (up to 1FFFFFFF) or a range "
                       "like 7E0-7EF")
                .arg(item);
        }
        if (dash < 0) {
            r.last = r.first;
        } else if (r.last < r.first) {
            return QStringLiteral("\"%1\" runs backward").arg(item);
        }
        ranges.push_back(r);
    }
    *out = std::move(ranges);
    return {};
}

QString FrameFilter::parseData(const QString& text, std::vector<int>* out,
                               bool* anchored)
{
    out->clear();
    *anchored = false;
    QString t = text.trimmed();
    bool anchor = false;
    if (t.startsWith(QLatin1Char('^'))) {
        anchor = true;
        t = t.mid(1).trimmed();
    }
    if (t.isEmpty()) {
        return anchor ? QStringLiteral("\"^\" needs bytes after it")
                      : QString();
    }
    std::vector<int> bytes;
    for (const QString& word : t.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        // "620D" reads as two bytes, as a copied payload would
        if (word.size() % 2 != 0) {
            return QStringLiteral(
                       "\"%1\" isn't whole bytes: two hex digits each, or ??")
                .arg(word);
        }
        for (int i = 0; i < word.size(); i += 2) {
            const QString pair = word.mid(i, 2);
            if (pair == QLatin1String("??")) {
                bytes.push_back(-1);
                continue;
            }
            bool ok = false;
            const uint value = pair.toUInt(&ok, 16);
            if (!ok) {
                return QStringLiteral(
                           "\"%1\" isn't a hex byte or ??").arg(pair);
            }
            bytes.push_back(static_cast<int>(value));
        }
    }
    *out = std::move(bytes);
    *anchored = anchor;
    return {};
}

QString FrameFilter::parseLengths(const QString& text,
                                  std::vector<LengthRange>* out)
{
    out->clear();
    if (text.trimmed().isEmpty()) {
        return {};
    }
    std::vector<LengthRange> ranges;
    for (const QString& raw : text.split(QLatin1Char(','))) {
        const QString item = raw.trimmed();
        if (item.isEmpty()) {
            return QStringLiteral("an empty item between commas");
        }
        LengthRange r;
        const qsizetype dash = item.indexOf(QLatin1Char('-'));
        const bool ok = dash < 0
                            ? decimalLength(item, &r.first)
                            : decimalLength(item.left(dash), &r.first) &&
                                  decimalLength(item.mid(dash + 1), &r.last);
        if (!ok) {
            return QStringLiteral(
                       "\"%1\" isn't a length in bytes (up to %2) or a range "
                       "like 1-7")
                .arg(item)
                .arg(ECUIO_FRAME_DATA_MAX);
        }
        if (dash < 0) {
            r.last = r.first;
        } else if (r.last < r.first) {
            return QStringLiteral("\"%1\" runs backward").arg(item);
        }
        ranges.push_back(r);
    }
    *out = std::move(ranges);
    return {};
}

QString FrameFilter::parseTimes(const QString& text, TimeRange* out)
{
    *out = TimeRange();
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        return {};
    }
    // Not const: each return moves it
    QString shape =
        QStringLiteral("\"%1\" isn't a range of seconds like 12.5-30, "
                       "12.5- (from then on) or -30 (up to then)")
            .arg(t);
    const qsizetype dash = t.indexOf(QLatin1Char('-'));
    if (dash < 0 || t.indexOf(QLatin1Char('-'), dash + 1) >= 0) {
        return shape;
    }
    const QString from = t.left(dash).trimmed();
    const QString to = t.mid(dash + 1).trimmed();
    TimeRange r;
    r.set = true;
    if ((from.isEmpty() && to.isEmpty()) ||
        (!from.isEmpty() && !seconds(from, &r.first)) ||
        (!to.isEmpty() && !seconds(to, &r.last))) {
        return shape;
    }
    if (r.last < r.first) {
        return QStringLiteral("\"%1\" runs backward").arg(t);
    }
    *out = r;
    return {};
}

int FrameFilter::terms() const
{
    return (ids.empty() ? 0 : 1) + (data.empty() ? 0 : 1) +
           (direction == AnyDirection ? 0 : 1) + (flag == AnyFlag ? 0 : 1) +
           (lengths.empty() ? 0 : 1) + (time.set ? 1 : 0);
}

bool FrameFilter::matches(uint32_t id, uint32_t flags, uint64_t timestampUs,
                          const uint8_t* payload, int length) const
{
    const bool extended = (flags & ECUIO_FRAME_FLAG_EXTENDED) != 0;
    if (!ids.empty()) {
        bool any = false;
        for (const IdRange& r : ids) {
            const bool in = r.masked
                                ? (id & r.mask) == (r.first & r.mask) &&
                                      (extended || (r.first <= kMaxStandardId &&
                                                    r.mask <= kMaxStandardId))
                                : id >= r.first && id <= r.last &&
                                      (extended || id <= kMaxStandardId);
            if (in) {
                any = true;
                break;
            }
        }
        if (!any) {
            return false;
        }
    }
    const bool sent = (flags & ECUIO_FRAME_FLAG_TX_ECHO) != 0;
    if ((direction == Received && sent) || (direction == Sent && !sent)) {
        return false;
    }
    switch (flag) {
    case Extended:
        if (!extended) return false;
        break;
    case Standard:
        if (extended) return false;
        break;
    case Fd:
        if ((flags & ECUIO_FRAME_FLAG_FD) == 0) return false;
        break;
    case Reassembled:
        if ((flags & ECUIO_FRAME_FLAG_REASSEMBLED) == 0) return false;
        break;
    case Truncated:
        if ((flags & ECUIO_FRAME_FLAG_TRUNCATED) == 0) return false;
        break;
    case Overflow:
        if ((flags & ECUIO_FRAME_FLAG_OVERFLOW) == 0) return false;
        break;
    case Error:
        if ((flags & ECUIO_FRAME_FLAG_ERROR) == 0) return false;
        break;
    default:
        break;
    }
    if (time.set && (timestampUs < time.first || timestampUs > time.last)) {
        return false;
    }
    if (!lengths.empty()) {
        bool any = false;
        for (const LengthRange& r : lengths) {
            if (length >= r.first && length <= r.last) {
                any = true;
                break;
            }
        }
        if (!any) {
            return false;
        }
    }
    if (data.empty()) {
        return true;
    }
    const int n = static_cast<int>(data.size());
    const int lastStart = anchored ? 0 : length - n;
    for (int start = 0; start <= lastStart && start + n <= length; ++start) {
        bool all = true;
        for (int i = 0; i < n; ++i) {
            if (data[static_cast<size_t>(i)] >= 0 &&
                payload[start + i] != data[static_cast<size_t>(i)]) {
                all = false;
                break;
            }
        }
        if (all) {
            return true;
        }
    }
    return false;
}
