// File/layer: app/framefilter.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: FrameFilterTest, BridgeTest.FrameFilter* (test_bridge)

#include "framefilter.h"

#include <QStringList>

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
        IdRange r;
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

int FrameFilter::terms() const
{
    return (ids.empty() ? 0 : 1) + (data.empty() ? 0 : 1) +
           (direction == AnyDirection ? 0 : 1) + (flag == AnyFlag ? 0 : 1);
}

bool FrameFilter::matches(uint32_t id, uint32_t flags, const uint8_t* payload,
                          int length) const
{
    const bool extended = (flags & ECUIO_FRAME_FLAG_EXTENDED) != 0;
    if (!ids.empty()) {
        bool any = false;
        for (const IdRange& r : ids) {
            if (id >= r.first && id <= r.last &&
                (extended || id <= kMaxStandardId)) {
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
