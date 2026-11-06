// File/layer: tests/packed_plugin.h | test helper, Qt6 Core
// Testing: none of its own; BridgeTest.Plugin* and tst_settings.qml use it
//
// The repository's example plugin (plugins/example-sim, ADR-0052) in its
// packed form, as the app imports one: the manifest with its fixtures
// left out (a phone loads data, it runs no checks) and each data file
// inlined under "files" by its relative name. Built as text, since core
// wants "havne_plugin" first and a QJsonObject sorts its keys.

#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>

#ifndef ECUIO_PLUGINS_DIR
#error "the test target defines ECUIO_PLUGINS_DIR"
#endif

namespace packed_plugin {

inline QByteArray readText(const QString& relative)
{
    QFile f(QStringLiteral(ECUIO_PLUGINS_DIR "/example-sim/") + relative);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// The example plugin packed; empty when its files can't be read.
// version replaces the manifest's own ("1.0.0") when given
inline QByteArray example(const QByteArray& version = QByteArray())
{
    QByteArray m = readText(QStringLiteral("manifest.json")).trimmed();
    const QByteArray base = readText(QStringLiteral("data/base.json"));
    const QByteArray my2019 = readText(QStringLiteral("data/my2019.json"));
    const int fixtures = m.indexOf("\"fixtures\"");
    const int comma = m.lastIndexOf(',', fixtures);
    const int close = m.indexOf(']', fixtures);
    if (m.isEmpty() || base.isEmpty() || my2019.isEmpty() || fixtures < 0 ||
        comma < 0 || close < 0 || !m.endsWith('}')) {
        return {};
    }
    m.remove(comma, close + 1 - comma);
    if (!version.isEmpty()) {
        m.replace("\"version\": \"1.0.0\"", "\"version\": \"" + version + "\"");
    }
    m.chop(1);
    return m.trimmed() + ",\n  \"files\": {\"data/base.json\": " + base +
           ", \"data/my2019.json\": " + my2019 + "}\n}\n";
}

} // namespace packed_plugin
