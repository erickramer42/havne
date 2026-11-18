// File/layer: app/choicerows.h | Qt6 Core, app layer, C++17, header-only
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// Which row a kept tick names. Core's rule, transcribed: the signal with
// that key, else the one signal with that name (ecuio_keys::find in
// core/src/signal_keys.h, what ecuio_signal_set_find answers by). Core is
// the source of truth; BridgeTest compares the two answer for answer. The
// model keeps its own copy because it outlives the session whose set core
// would search.

#ifndef CHOICEROWS_H
#define CHOICEROWS_H

#include <QByteArray>
#include <QHash>

class ChoiceRows
{
public:
    void reserve(qsizetype rows)
    {
        m_byKey.reserve(rows);
        m_byName.reserve(rows);
    }

    // The next row in the set's order: its key ("" for a row with none,
    // which no text finds by key) and its name
    void add(const QByteArray& key, const QByteArray& name)
    {
        if (!key.isEmpty() && !m_byKey.contains(key)) {
            m_byKey.insert(key, m_rows);
        }
        const auto named = m_byName.find(name);
        if (named == m_byName.end()) {
            m_byName.insert(name, m_rows);
        } else {
            *named = Several;
        }
        ++m_rows;
    }

    // The row text names, or -1: no row, an empty text, or a name several
    // rows have and no key
    int rowOf(const QByteArray& text) const
    {
        if (text.isEmpty()) {
            return -1;
        }
        const auto keyed = m_byKey.constFind(text);
        if (keyed != m_byKey.cend()) {
            return *keyed;
        }
        const auto named = m_byName.constFind(text);
        return named != m_byName.cend() && *named != Several ? *named : -1;
    }

private:
    enum : int { Several = -2 };
    QHash<QByteArray, int> m_byKey;  // the first row with each key
    QHash<QByteArray, int> m_byName; // a name's one row, or Several
    int m_rows = 0;
};

#endif // CHOICEROWS_H
