// File/layer: app/signalstore.h | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.SignalStore* and .SignalFile* (test_bridge,
//          each under a HAVNE_SETTINGS_DIR of its own): a copy kept by
//          its hash and reused, the loader's words without a path, a file
//          that no longer loads at start-up, one profile to one file, the
//          registration followed, and a copy deleted only once its
//          profile is unregistered; the page's side in tst_settings.qml
//
// The vehicles' signal files (ADR-0051), kept by the app: each file a
// user imports is read by core's loader from its bytes, and when it
// loads, a copy is kept as <sha256>.json in the store's folder
// (docs/signal-files.md, "Where Havne keeps them"), named by its
// content's SHA-256 so two vehicles holding one file share one copy and
// a recording's hash names the same bytes. The vehicle book (ADR-0027)
// says which files each vehicle has; the store loads them, registers
// each as its profile (user.<slug>), and deletes a copy no vehicle
// keeps.

#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <map>

#include "ecuio/signals.h"

class SignalStore
{
public:
    // The folder: <HAVNE_SETTINGS_DIR>/signals when that's set (a test's
    // or a second instance's), else <AppDataLocation>/signals. Read at
    // each call, so a test that sets the variable is the store's.
    static QString root();

    SignalStore() = default;
    ~SignalStore();
    SignalStore(const SignalStore&) = delete;
    SignalStore& operator=(const SignalStore&) = delete;

    // A file's bytes into the store: { ok, words, sha256, name, profile,
    // count, kept }. ok when core's loader reads it, its copy then
    // written unless one is there already (kept true). words, when it
    // doesn't, are the loader's ("line 12, column 9: unknown key
    // 'scael'"), never a path. Registers nothing: use() does.
    QVariantMap import(const QByteArray& bytes);

    // Registers exactly these files' profiles, each loaded from its copy
    // the first time, and unregisters every other this store registered.
    // One entry per sha, in order: { sha256, ok, words, name, profile,
    // count }; not ok for a copy that's missing or no longer loads, a
    // profile another of these files makes, or one the registry refuses
    // (its profile then isn't offered).
    QVariantList use(const QStringList& shas);

    // Unregisters, then deletes, every copy that keep doesn't name; the
    // folder too when it ends empty. Returns how many copies went.
    int deleteUnused(const QStringList& keep);

    // Everything unregistered and freed. The bridge calls it once its
    // session is destroyed: until then a session may decode with a set
    // this store holds (profile.h: the registry keeps the pointer).
    void close();

private:
    struct Held {
        ecuio_signal_file_t* file = nullptr;
        QString name;
        QString profile;
        int count = 0;
    };

    static QString pathOf(const QString& sha);
    static QVariantMap entryOf(const QString& sha, const Held& h);
    // The file by its hash: held already, or loaded from its copy. nullptr
    // with *words saying why not
    const Held* load(const QString& sha, QString* words);
    // Takes a loaded file in; false when it didn't load (*words the
    // loader's)
    bool take(const QString& sha, const QByteArray& bytes, QString* words);

    // Every file loaded, kept until close(): a profile unregistered may
    // still be some session's set, so nothing is freed before then
    std::map<QString, Held> m_held;
    QSet<QString> m_registered; // shas whose profile is registered
};
