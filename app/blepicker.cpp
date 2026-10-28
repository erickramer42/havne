// File/layer: app/blepicker.cpp | Qt6 Core, app layer, C++17
// Testing: UNIT — BridgeTest.BlePicker* (test_bridge)

#include "blepicker.h"

namespace {

const QString kNordicUart =
    QStringLiteral("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
const QString kNordicRx =
    QStringLiteral("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
const QString kNordicTx =
    QStringLiteral("6e400003-b5a3-f393-e0a9-e50e24dcca9e");

bool writable(const BleCharacteristic& c)
{
    return c.write || c.writeNoResponse;
}

bool notifying(const BleCharacteristic& c)
{
    return c.notify || c.indicate;
}

int indexOf(const BleService& s, const QString& uuid)
{
    for (size_t i = 0; i < s.characteristics.size(); ++i) {
        if (bleUuid(s.characteristics[i].uuid) == uuid) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The service with this UUID, if its write and notify characteristics are
// there and can do their part
BlePick named(const std::vector<BleService>& services, const QString& service,
              const QString& write, const QString& notify)
{
    for (size_t i = 0; i < services.size(); ++i) {
        const BleService& s = services[i];
        if (bleUuid(s.uuid) != service) {
            continue;
        }
        const int w = indexOf(s, write);
        const int n = indexOf(s, notify);
        if (w >= 0 && n >= 0 && writable(s.characteristics[size_t(w)]) &&
            notifying(s.characteristics[size_t(n)])) {
            return {static_cast<int>(i), w, n};
        }
    }
    return {};
}

} // namespace

QString bleUuid(const QString& uuid)
{
    QString u = uuid.trimmed().toLower();
    if (u.startsWith(QLatin1Char('{')) && u.endsWith(QLatin1Char('}'))) {
        u = u.mid(1, u.size() - 2);
    }
    if (u.size() == 4) {
        return QStringLiteral("0000%1-0000-1000-8000-00805f9b34fb").arg(u);
    }
    return u;
}

BlePick pickBleService(const std::vector<BleService>& services)
{
    const BlePick nordic = named(services, kNordicUart, kNordicRx, kNordicTx);
    if (nordic.found()) {
        return nordic;
    }
    const BlePick fff0 =
        named(services, bleUuid("fff0"), bleUuid("fff2"), bleUuid("fff1"));
    if (fff0.found()) {
        return fff0;
    }
    const BlePick ffe0 =
        named(services, bleUuid("ffe0"), bleUuid("ffe1"), bleUuid("ffe1"));
    if (ffe0.found()) {
        return ffe0;
    }
    for (size_t i = 0; i < services.size(); ++i) {
        int w = -1;
        int n = -1;
        const auto& cs = services[i].characteristics;
        for (size_t j = 0; j < cs.size(); ++j) {
            if (w < 0 && writable(cs[j])) {
                w = static_cast<int>(j);
            }
            if (n < 0 && notifying(cs[j])) {
                n = static_cast<int>(j);
            }
        }
        if (w >= 0 && n >= 0) {
            return {static_cast<int>(i), w, n};
        }
    }
    return {};
}
