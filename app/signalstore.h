// File/layer: app/signalstore.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge); QML: QmlTest.settings (test_qml)
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
    // extends, count, own, kept }: extends the built-in set it adds to
    // ("obd2", or ""), count its set's signals and own those less the
    // base's. ok when core's loader reads it, its copy then
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

    // A kept copy's text, "" when there's none: the vehicle's own file,
    // which the form adds to and saves again through import()
    static QString text(const QString& sha);

    // The loaded file whose set this is (a session's,
    // ecuio_session_signal_set), or nullptr for a set no file of the
    // store's holds: what a recording copies as signals.json (ADR-0051
    // section 4). Valid until close().
    const ecuio_signal_file_t* fileFor(const ecuio_signal_set_t* set) const;

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
        QString extends; // the built-in set it adds to ("obd2"), or ""
        int count = 0;   // the set's signals, the base's included
        int own = 0;     // the file's own: count less the base's
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
