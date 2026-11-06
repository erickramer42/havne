// File/layer: app/pluginstore.h | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.Plugin* (test_bridge, under a
//          HAVNE_SETTINGS_DIR of its own); the page's side in
//          tst_settings.qml
//
// The app's plugins (ADR-0052, the data tier): each packed plugin a user
// imports (<id>.havne-plugin.json) is read by core from its bytes, and
// when it loads, a copy is kept as <sha256>.havne-plugin.json in the
// store's folder, as signal files are (docs/signal-files.md, "Where
// Havne keeps them"). Plugins are the app's, not a vehicle's: a plugin
// covers many cars, and its manifest's match proposes variants for one
// by its VIN (ADR-0052 section 5). Every variant of every plugin loaded
// is registered as its profile, plugin.<id>.<variant>, so a URI naming
// one connects. One version per id: an import of an id already loaded
// replaces it, once no session decodes with it.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <map>

#include "ecuio/plugin.h"

class PluginStore
{
public:
    // <HAVNE_SETTINGS_DIR>/plugins when the variable is set (the tests),
    // else <AppDataLocation>/plugins
    static QString root();

    PluginStore() = default;
    ~PluginStore();
    PluginStore(const PluginStore&) = delete;
    PluginStore& operator=(const PluginStore&) = delete;

    // Every kept copy loaded and its variants registered, once (later
    // calls only list): one entry a plugin, by name, { sha256, ok, words,
    // id, name, version, variants, profiles }; a copy that no longer
    // loads is { sha256, ok false, words }
    QVariantList all();

    // A package's bytes into the store: the entry above, plus kept (a
    // copy of these bytes was there) and replaced (the name of the
    // version it replaced, "" for none). words, when it's refused, are
    // core's ("line 12, column 9: ..."), never a path; an id whose loaded
    // version a session decodes with is refused, nothing changed
    QVariantMap import(const QByteArray& bytes);

    // Its profiles unregistered and the plugin freed, then its copy
    // deleted: { ok, words }, refused while a session decodes with it
    QVariantMap remove(const QString& sha);

    // The variants the plugins loaded propose for a VIN (ADR-0052
    // section 5), in the plugins' order then the manifest's: { sha256,
    // id, name, variant, profile }; none for text that isn't a VIN
    QVariantList matches(const QString& vin, int latestModelYear) const;

    // Every plugin freed. The bridge calls it once its session is
    // destroyed: until then a session may decode with one of their sets
    void close();

private:
    struct Held {
        ecuio_plugin_t* plugin = nullptr;
        QString id;
        QString name;
        QString version;
        QStringList variants;
        QStringList profiles;
    };

    static QString pathOf(const QString& sha);
    static QVariantMap entryOf(const QString& sha, const Held& h);
    // Loads a package and registers every variant: false with *words
    // saying why not (another copy of its id loaded, a profile refused)
    bool take(const QString& sha, const QByteArray& bytes, QString* words);

    std::map<QString, Held> m_held;       // by the copy's SHA-256
    std::map<QString, QString> m_refused; // copies that didn't load, why
    bool m_loaded = false;
};
