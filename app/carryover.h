// File/layer: app/carryover.h | Qt6 Core, app layer, C++17
// Testing: UNIT — BridgeTest.CarryOver* (test_bridge): the preferences
//          copied once, never over a store that has some, and the
//          bridge's recent recordings reading the earlier folder too
//
// What the product's earlier name left on the user's machine comes along
// after a rename (ADR-0001's appendix of 2026-10-28): its preferences are
// copied into the current name's store once, and its recordings folder is
// read beside the current one (SessionBridge::recentRecordings). Where
// the two names are the same, both do nothing.

#pragma once

#include <QString>

// The product's name before the rename: its QSettings organization and
// application, and its folder under Documents
QString earlierProductName();

// Copies every key of the earlier store (fromOrg, fromApp) into the
// current one (toOrg, toApp), in QSettings' default format and the user's
// scope, once: only when the names differ, the earlier store holds keys
// and the current one holds none. The earlier store is left as it was, so
// an older build still finds its own. Returns how many keys it copied
int carryOverSettings(const QString& fromOrg, const QString& fromApp,
                      const QString& toOrg, const QString& toApp);
