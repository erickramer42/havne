// File/layer: app/pluginstore.h | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.Plugin*, BridgeTest.OneStore* (test_bridge,
//          under a HAVNE_SETTINGS_DIR of its own); the page's side in
//          tst_settings.qml
//
// The app's one store (ADR-0052, ADR-0064): every import ends here as a
// packed plugin. A package (<id>.havne-plugin.json) is kept as it is; a
// signal file is wrapped by core into a plugin of one variant, and an
// OBDb set is converted to a signal file first. Core reads each from its
// bytes, and when it loads, a copy is kept as <sha256>.havne-plugin.json
// in the store's folder. Every variant of every plugin loaded is
// registered as its profile, plugin.<id>.<variant>, so a URI naming one
// connects. One version per id.
//
// What only this device knows sits in the store's index, store.json
// beside the copies, never in a package: which plugins are the user's
// own (mine), the vehicle an import was made for (importedFor) and the
// signal file each variant was made from (wrappedFrom, the file's
// SHA-256 to the variant's id). Only a plugin of the user's own takes a
// new, changed or removed variant, and a package with its id replaces
// it only as a later version of the same plugin: the same lineage, a
// higher version.
//
// The index is never written over when it can't be trusted: one that
// can't be read (not JSON, too large, unreadable) is moved aside as
// store.json.bad-N and the store starts with no facts; one of a later
// format is read and left as it is. Each change is made to the file as
// it is then, so two running apps don't undo each other's facts.

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <map>
#include <vector>

#include "ecuio/plugin.h"

class PluginStore
{
public:
    // The most variants one vehicle picks, so the profile row stays a row
    static constexpr int MaxPicks = 16;

    // <HAVNE_SETTINGS_DIR>/plugins when the variable is set (the tests),
    // else <AppDataLocation>/plugins
    static QString root();

    PluginStore() = default;
    ~PluginStore();
    PluginStore(const PluginStore&) = delete;
    PluginStore& operator=(const PluginStore&) = delete;

    // Where the store's own lines go (an index moved aside, one it
    // leaves alone); none until set. Called on the store's thread
    void setLog(std::function<void(const QString&)> log)
    {
        m_log = std::move(log);
    }

    // Every kept copy loaded and its variants registered, once (later
    // calls only list): one entry a plugin, by name, { sha256, ok, words,
    // id, name, version, lineage, variants, profiles, extends, counts,
    // owns, replacedNames, mine, importedFor, obdb }. extends, counts, owns and
    // replacedNames run with variants: the built-in set a variant adds to
    // ("obd2" or ""), its set's signals, the signals it adds to the base's
    // and the names of the base's it replaces (variantshape.h; a replaced
    // name is in neither the count's base part nor owns). obdb: the plugin is a
    // converted OBDb set. A copy that no longer loads is { sha256, ok
    // false, words }
    QVariantList all();

    // A package's bytes into the store: the entry above, plus kept (a
    // copy of these bytes was there), replaced (the version it took the
    // place of, "" for none), replacedProfiles (that version's profiles,
    // so a pick of one the new version lacks can be moved) and updated
    // (the version of the user's own plugin it updated in place, "" for
    // none). words, when it's refused, are core's ("line 12, column 9:
    // ..."), never a path. Refused with nothing changed: an id whose
    // loaded version a session decodes with (inUse true); an import
    // whose copy can't be kept; and, for the id of a plugin of the
    // user's own, any package that isn't a later version of it (another
    // lineage or none, or the same lineage at the same or a lower
    // version). The version it replaces goes once the import has loaded;
    // one that doesn't is loaded again. A package whose id has facts and
    // no plugin loaded (the copy they were of no longer loads) comes in
    // with none: the facts were another plugin's
    QVariantMap import(const QByteArray& bytes);

    // What a signal file would be as a plugin, nothing kept: { ok, words,
    // name, id, variant }, id the plugin id a wrap gives it and variant
    // the id a variant made from it takes; words are core's
    static QVariantMap describe(const QByteArray& file);

    // A signal file wrapped into a plugin of the user's own (ADR-0064 Q2):
    // import's entry. vehicle, when given, is the vehicle it was imported
    // for; variant names its one variant ("" is core's "base"). The same
    // file again finds the plugin it made (kept), with words when that
    // plugin has changed since. An id the store has is numbered -2 to
    // -99. A converted OBDb set imported for a vehicle goes to that
    // vehicle's earlier conversion of the same name: in its place while
    // that plugin has one variant, and as its variant (merged true, put's
    // rules) once the user has added others, so nothing of theirs goes.
    // With no vehicle that rule isn't looked at: the set is a plugin of
    // its own, unless the same bytes were kept before, when it finds the
    // plugin they made (by wrappedFrom or by its bytes). The same bytes
    // for another vehicle find the plugin and never move importedFor,
    // which stays the first vehicle's
    struct Wrap {
        QString vehicle;
        QString variant;
    };
    QVariantMap wrap(const QByteArray& file, const Wrap& how);

    // A variant of a plugin of the user's own made from a signal file,
    // added or put in the place of the one with that id (variant "" is
    // the file's name as core cuts it), and one removed (never the
    // last): import's entry for the new version, plus notes, core's words
    // for the user when an ECU of the file's took another id. Refused for
    // a plugin that isn't the user's own. The lineage stays and the
    // version's minor rises
    QVariantMap put(const QString& sha, const QString& variant,
                    const QByteArray& file);
    QVariantMap drop(const QString& sha, const QString& variant);

    // A vehicle forgotten, or every vehicle: importedFor naming it goes
    // from each plugin, so a vehicle later given that id inherits no
    // import (its list's own plugins, the OBDb set it would replace). The
    // plugins stay, and stay the user's own. { ok, words, count }: count
    // the plugins it went from, words as markMine's
    QVariantMap forgetVehicle(const QString& vehicle);
    QVariantMap forgetVehicles();

    // The plugin marked as the user's own: { ok, words }. words with ok
    // say the index couldn't be written, so the mark holds only while
    // the app runs
    QVariantMap markMine(const QString& sha);

    // Its profiles unregistered and the plugin freed, then its copy and
    // its index entry deleted: { ok, words }, refused while a session
    // decodes with it
    QVariantMap remove(const QString& sha);

    // The kept copy's bytes, what Export plugin writes: empty, with
    // words why, for a plugin the store doesn't hold or a copy that no
    // longer hashes to its name
    QByteArray packed(const QString& sha, QString* words) const;

    // A variant's signal file (ADR-0064 Q7): its files merged and written
    // as one, with a notes line naming the plugin, its version and the
    // variant, which loads anywhere a signal file does, under
    // user.<the plugin's name>. Empty, with words why, for a plugin or a
    // variant the store doesn't hold. Save as a signal file writes it
    QByteArray variantBytes(const QString& sha, const QString& variant,
                            QString* words);
    // The same as text, for the form: { ok, words, text }
    QVariantMap variantText(const QString& sha, const QString& variant);

    // What Export plugin says before its save dialog (ADR-0064 Q7):
    // { ok, words, note, shareAlike }, shareAlike a { variant, license,
    // words } for each variant whose source's license is Creative
    // Commons' share-alike, in the plugin's order, its words those
    // havne-cli plugins check gives; note those words one a line, ""
    // when no variant is
    QVariantMap exportNote(const QString& sha);

    // Copy to my plugin (ADR-0064 Q8): a variant's signal file into a
    // plugin of the user's own, a new one (into "": wrap's rules, for no
    // vehicle, variant newVariant) or one held (put's rules: into must
    // be the user's own), its source kept. Refused into the plugin it is
    // in, and as a variant into already has (newVariant, else the file's
    // name as core cuts it): a copy never takes a variant's place. wrap's
    // or put's answer, its notes ending with the line that the copy
    // won't follow the original's updates
    QVariantMap copyVariant(const QString& sha, const QString& variant,
                            const QString& into, const QString& newVariant);

    // The variants the plugins loaded propose for a VIN (ADR-0052
    // section 5), in the plugins' order then the manifest's: { sha256,
    // id, name, variant, profile }; none for text that isn't a VIN
    QVariantList matches(const QString& vin, int latestModelYear) const;

    // Every variant of every plugin as a vehicle's list offers it
    // (ADR-0064 Q5): { sha256, id, name, version, variant, profile,
    // extends, mine, forThisCar, picked }. forThisCar: the VIN proposes
    // it or its plugin was imported for this vehicle; those come first,
    // each group in the plugins' order then the manifest's
    QVariantList offered(const QString& vehicle, const QString& vin,
                         int latestModelYear, const QStringList& picks);

    // A vehicle's picks as the profile row shows them, in their order:
    // { profile, sha256, id, name, version, variant, extends, missing,
    // standIn }. missing: no plugin loaded has the variant. standIn, on
    // an adapter only: the first pick that extends obd2, which takes
    // Default's place
    QVariantList pickRow(const QStringList& picks, bool adapter);

    // picks with profile added last: { ok, words, picks }. Refused, the
    // list as it was, for a variant no plugin loaded has and past
    // MaxPicks; a profile picked already changes nothing
    QVariantMap pick(const QStringList& picks, const QString& profile);
    static QStringList unpick(const QStringList& picks, const QString& profile);

    // A vehicle's picks the store has, as a layout file names what it
    // needs (ADR-0064, "Dashboards"), in the picks' order: { profile,
    // name, sha256, from }. sha256 is the variant's written file's (the
    // hash a recording names); from is the SHA-256 of each signal file
    // the variant was made from (wrappedFrom), which a layout saved
    // before the move names. name is the plugin's, with ": <variant>"
    // where it has several. A pick no plugin loaded has gives no entry
    QVariantList pickFiles(const QStringList& picks);

    // The variants made from the signal file with this SHA-256
    // (wrappedFrom), among the plugins loaded: { id, variant, profile,
    // wrapped }, what a layout saved before the merge and an address
    // naming user.<slug> are mapped by. wrapped: the plugin was made
    // from this file (its lineage is the file's), not given it as a
    // variant; those come first, then by id
    QVariantList wrappedFrom(const QString& fileSha);

    // The index's mark that the old signal store was moved in, set last
    // by the migration; false when the index can't be written. An index
    // moved aside loses the mark, so the migration also looks for what
    // it wrote (the vehicle book's new key) before it runs again
    bool migrated();
    bool setMigrated();

    // The variant whose set is set, as one signal file (its files merged,
    // the plugin's id, version and variant on its header line), for a
    // recording's signals.json; nullptr for a set no plugin here holds
    const ecuio_signal_file_t* fileFor(const ecuio_signal_set_t* set) const;

    // Every plugin freed. The bridge calls it once its session is
    // destroyed: until then a session may decode with one of their sets
    void close();

    // Test seam: the next load of the copy with this hash fails, as a
    // registration refused would, so a replace's fallback can be reached
    void failLoadForTesting(const QString& sha) { m_failLoadForTesting = sha; }
    // Test seam: the next write of the index fails, as a full disk's
    // would, so what a change does without its facts can be reached. A
    // call that writes nothing (a refusal, nothing to change) leaves it set
    void failIndexWriteForTesting() { m_failIndexWriteForTesting = true; }

private:
    struct Held {
        ecuio_plugin_t* plugin = nullptr;
        QString id;
        QString name;
        QString version;
        QString lineage;
        QStringList variants;
        QStringList profiles;
        QStringList extends; // by variant: "obd2" or ""
        QVariantList counts; // by variant: the set's signals
        QVariantList owns;   // by variant: the signals it adds to the base's
        QVariantList replacedNames; // by variant: the base signals it replaces, by name
        bool obdb = false;   // a converted OBDb set
    };
    using HeldMap = std::map<QString, Held>;

    static QString pathOf(const QString& sha);
    static QString indexPath();
    QVariantMap entryOf(const QString& sha, const Held& h) const;
    // Loads a package and registers every variant: false with *words
    // saying why not (another copy of its id loaded, a profile refused)
    bool take(const QString& sha, const QByteArray& bytes, QString* words);
    // A package that loads into the store, in the place of the loaded
    // plugin with its id when there is one: import's map, whatever the
    // lineage says. retry ends the words of a refusal while a session
    // decodes with the one it replaces ("import it again")
    QVariantMap install(const QByteArray& bytes, const QString& sha,
                        const QString& id, const QString& retry);
    // What install gave, with the index saying the plugin is the user's
    // own, imported for vehicle (when given, and none is recorded yet)
    // and its variant made from the signal file with the hash fileSha
    // (when given, in the place of that variant's earlier file); a
    // refusal passes through
    struct Note {
        QString vehicle;
        QString fileSha;
        QString variant;
    };
    QVariantMap noted(QVariantMap installed, const QString& id,
                      const Note& note);
    // An entry for a copy that is here already: kept, nothing replaced
    QVariantMap keptEntry(const QString& sha) const;
    // put's work on a plugin known to be the user's own
    QVariantMap putInto(const QString& sha, const QString& variant,
                        const QByteArray& file, const QString& vehicle);
    // The loaded plugins by name, then by id
    std::vector<const HeldMap::value_type*> byName() const;
    HeldMap::const_iterator heldWithId(const QString& id) const;
    HeldMap::const_iterator heldWithProfile(const QString& profile,
                                            int* variant) const;

    // The index: read at the first use; each change is made to the file
    // as it is then and written whole. Keys this version doesn't know
    // are kept as they are
    void loadIndex();
    // The file moved aside as store.json.bad-N: false when it can't be
    bool indexAside();
    // One change to the index, written: false, the change held for the
    // running app only, when the file can't be written or mustn't be. A
    // change that returns false has nothing to change: nothing is written,
    // and the answer is true
    bool changeIndex(const std::function<bool(QJsonObject&)>& change);
    bool changeFacts(const QString& id,
                     const std::function<void(QJsonObject&)>& change);
    // A plugin's facts after an install: none for an id that came in
    // new, and no file named for a variant it no longer has
    void settleFacts(const QString& id, bool cameNew,
                     const QStringList& variants);
    QJsonObject facts(const QString& id) const;
    bool isMine(const QString& id) const;
    void say(const QString& line) const;

    HeldMap m_held;                       // by the copy's SHA-256
    std::map<QString, QString> m_refused; // copies that didn't load, why
    QJsonObject m_index;
    bool m_indexLoaded = false;
    bool m_indexUnsaved = false; // a change only memory holds: it wins
    bool m_indexStuckSaid = false;
    bool m_loaded = false;
    QString m_failLoadForTesting; // failLoadForTesting's hash, once
    bool m_failIndexWriteForTesting = false; // once
    // importedFor taken from the plugins naming *vehicle, from every
    // plugin for nullptr
    QVariantMap forgetImports(const QString* vehicle);
    std::function<void(const QString&)> m_log;
};
