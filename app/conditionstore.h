// File/layer: app/conditionstore.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// The app's one set of conditions (ADR-0063 decision 4): a condition file
// (docs/condition-files.md) kept as conditions.json in the settings folder,
// beside the plugin store. Every file is read by core's loader before it
// is kept, so the app never holds one core refuses; a save writes a
// temporary file and renames it over the old one, so a failed write leaves
// the old file whole.

#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>

#include <memory>

#include "ecuio/conditions.h"

struct ConditionSetFree {
    void operator()(ecuio_condition_set_t* set) const
    {
        ecuio_condition_set_free(set);
    }
};
using ConditionSetPtr =
    std::unique_ptr<ecuio_condition_set_t, ConditionSetFree>;

// A set loaded from bytes, or nullptr with *words core's detail (never a
// path)
ConditionSetPtr loadConditionSet(const QByteArray& bytes, QString* words);

class ConditionStore
{
public:
    // <HAVNE_SETTINGS_DIR>/conditions.json when that's set (a test's or a
    // second instance's), else <AppDataLocation>/conditions.json; read at
    // each call, as PluginStore::root is
    static QString path();

    // Reads the file: true with no set when there's none; false, no set
    // kept, with *words why, for one that can't be read or core refuses
    // (whose bytes refused() keeps)
    bool load(QString* words);

    // A file's bytes in place of the kept one: { ok, words, conditions,
    // name, aside }. Refused, with core's words and nothing written, for a
    // file core refuses or one past ECUIO_CONDITION_FILE_MAX_BYTES; refused
    // with the write's words, the old file kept, when it can't be written (a
    // file moved aside first is named in them: it is then the only copy).
    // A file load couldn't read or take for its size is first moved to
    // conditions.json.bad-N, whose name aside carries ("" when none moved)
    QVariantMap save(const QByteArray& bytes);

    // Fails the next save's write once, after any move aside, as a full
    // disk would: for tests only
    void failNextWriteForTesting() { m_failWrite = true; }

    // The kept file's bytes ("" with none) and its set (nullptr with none)
    const QByteArray& bytes() const { return m_bytes; }
    const ecuio_condition_set_t* set() const { return m_set.get(); }
    // The file core refused at the last load, so an editor can mend it;
    // "" once a save succeeds
    const QByteArray& refused() const { return m_refused; }

private:
    // Moves an unreadable or over-bound file to conditions.json.bad-N
    // (the first free N), so the save that follows doesn't write over it:
    // *to is its name, "" when the file is gone (moved by hand since the
    // load). false when it can't be moved
    bool moveAside(QString* to) const;

    bool m_failWrite = false;
    bool m_unreadable = false; // the file is there, and load couldn't take it
    QByteArray m_bytes;
    ConditionSetPtr m_set;
    QByteArray m_refused;
};
