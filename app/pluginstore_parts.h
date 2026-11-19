// File/layer: app/pluginstore_parts.h | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.Plugin*, BridgeTest.OneStore* (test_bridge)
//
// What the plugin store's source files share: the index's keys more
// than one of them reads, a package's words for itself, and the
// answers' helpers. Internal: only the store's own .cpp files include it.

#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>

#include "ecuio/core.h"

namespace plugin_store {

const QString kMine = QStringLiteral("mine");
const QString kImportedFor = QStringLiteral("importedFor");
const QString kWrappedFrom = QStringLiteral("wrappedFrom");
const QString kReplacedProfiles = QStringLiteral("replacedProfiles");

// What a package says of itself
struct Named {
    QString id;
    QString name;
    QString version;
    QString lineage;
};

// The bytes' SHA-256 in hex
QString shaOf(const QByteArray& bytes);
// Core's words: the detail, else the status
QString coreWords(ecuio_status_t rc);
// An answer of ok false with these words
QVariantMap refusal(const QString& words);
// A package's own words for itself, read and freed; false with *words
bool nameOf(const QByteArray& bytes, Named* out, QString* words);

} // namespace plugin_store
