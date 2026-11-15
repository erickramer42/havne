// File/layer: app/storemigration.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.OneStoreMigration* (test_bridge, a scratch
//          ini and a HAVNE_SETTINGS_DIR of its own)
//
// The vehicles' signal files moved into the plugin store, once (ADR-0064
// Q6). What an older Havne reads is only read: the vehicle book under
// vehicles/document and the copies in signals/. What the move writes is
// new: a plugin of the user's own for each file the book names, the
// book's format 2 under vehicles/book, the bytes of a file that didn't
// move under plugins/unmigrated/, and the store's mark, last. The saved
// addresses (session/uri, session/recentUris) alone change in place: a
// profile=user.<slug> naming a moved file becomes its plugin's profile.
//
// Two facts decide what a start does. vehicles/book holding text: the
// files are moved and the book is the user's to edit, so neither is
// done again, and only the addresses and the mark are left. The store's
// mark: nothing is left. A stop before the book is written leaves the
// old state whole, and the next start moves the files again and finds
// each plugin it made.
//
// A file is moved when the store names a variant made from it
// (PluginStore::wrappedFrom), judged once every file has been through
// the wrap, since a later file can take an earlier one's place in a
// plugin. That one rule gives the book's picks and the addresses' new
// profiles, so a run that only finishes the addresses agrees with the
// run that wrote the book.
//
// The book is carried, not judged: each entry keeps every key it has but
// signals and plugin, which become picks (the files' profiles in their
// order, then the plugin pick); own becomes the own file's profile; uri
// is text, "" when it wasn't. A list element that isn't an object is
// left out. A file that didn't move is listed under the entry's
// unmigrated as { sha256, name, words }, with kept false when its bytes
// couldn't be copied and so are only in the old folder: that folder's
// removal must spare every copy a kept false names. With an old book
// that can't be read no entry exists, and the files that didn't move
// are listed the same way under the book's own unmigrated, kept false
// each, since their bytes stay where they are. VehicleBook.qml judges
// the result, and tests/data/vehiclebook holds both formats for the two
// readers.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

class PluginStore;
class QSettings;

class StoreMigration
{
public:
    struct Report {
        bool ran = false;     // false: the mark was set, nothing was read
        bool stopped = false; // the test seam ended it
        bool marked = false;  // the store's mark is written
        int made = 0;         // files that became a new plugin
        int kept = 0;         // files whose plugin was here already
        int unmigrated = 0;   // files the book names that didn't move
        int unnamed = 0;      // copies no vehicle lists, left where they are
        int addresses = 0;    // saved addresses given their new profile
        qint64 elapsedMs = 0;
        QStringList lines; // for the activity log: no path, no VIN
    };

    // The move, or what is left of it. prefs is the app's preferences (a
    // test's scratch ini); store is the bridge's, whose folder and the
    // old one's follow HAVNE_SETTINGS_DIR. Called on the GUI thread
    // before any page reads the book
    static Report run(QSettings& prefs, PluginStore& store);

    // <HAVNE_SETTINGS_DIR>/signals, else <AppDataLocation>/signals: the
    // old store's copies, <sha256>.json
    static QString oldSignalsRoot();

    // The bytes of the files that didn't move, <sha256>.json under
    // <the plugin store's folder>/unmigrated, named by the hash the book
    // has (a copy changed since it was kept no longer hashes to it): the
    // hashes there; one's bytes for a Save, empty with words why not;
    // and one removed, true when it is gone
    static QString unmigratedRoot();
    static QStringList unmigrated();
    static QByteArray unmigratedBytes(const QString& sha, QString* words);
    static bool removeUnmigrated(const QString& sha);

    // The old folder's copy of a file whose bytes weren't kept (a record
    // with kept false), as it is there now: empty with words why not,
    // for no copy and for one over OldCopyMaxBytes. Never removed
    static constexpr qint64 OldCopyMaxBytes = 64 * 1024 * 1024;
    static QByteArray oldCopy(const QString& sha, QString* words);

    // Test seam: the next run returns, as a killed app would, after the
    // nth file is wrapped (File), after every file is (Wrapped), after
    // the unmigrated bytes are copied (Copied), after the book is
    // written (Book) or after the addresses are (Addresses). None ends it
    enum class Step { None, File, Wrapped, Copied, Book, Addresses };
    static void stopAfterForTesting(Step step, int file = 0);
};
