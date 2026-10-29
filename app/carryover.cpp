// File/layer: app/carryover.cpp | Qt6 Core, app layer, C++17
// Testing: UNIT — BridgeTest.CarryOver* (test_bridge)

#include "carryover.h"

#include <QSettings>
#include <QStringList>

QString earlierProductName()
{
    return QStringLiteral("QtTune");
}

int carryOverSettings(const QString& fromOrg, const QString& fromApp,
                      const QString& toOrg, const QString& toApp)
{
    if (fromOrg == toOrg && fromApp == toApp)
        return 0;
    // Without fallbacks: only the user's own store for each name, never
    // the organization's or the system's keys
    QSettings to(QSettings::defaultFormat(), QSettings::UserScope, toOrg,
                 toApp);
    to.setFallbacksEnabled(false);
    if (!to.allKeys().isEmpty())
        return 0;
    QSettings from(QSettings::defaultFormat(), QSettings::UserScope, fromOrg,
                   fromApp);
    from.setFallbacksEnabled(false);
    const QStringList keys = from.allKeys();
    for (const QString& key : keys)
        to.setValue(key, from.value(key));
    to.sync();
    return static_cast<int>(keys.size());
}
