// File/layer: app/conditionmodel.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// The conditions as the app shows them (ADR-0063 section 9): one row per
// condition of the set the bridge holds, in the file's order. What a row
// is comes from the file and, while a session judges it, from the session:
// each term bound to the session's signals (ecuio_condition_set_resolve)
// and the state core keeps (ecuio_session_get_condition_states), applied
// on the bridge's tick. A value is shown in the display unit
// (ecuio_display_convert), from the unit of the def the term that settled
// it binds; core's own value stays in the def's unit.

#pragma once

#include <QAbstractListModel>
#include <QString>

#include <map>
#include <vector>

#include "ecuio/conditions.h"
#include "ecuio/signals.h"

class ConditionModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        NameRole,
        EnabledRole, // false for "enabled": false
        LevelRole,   // "warning", "danger", or "" for no alert
        UsesRole,    // a list of "alert", "record", "mark"
        SoundRole,
        BannerRole,
        LiveRole,  // a session judges it; the state roles below
                   // read off, unjudged, without one
        StateRole, // "on" or "off"
        StaleRole,
        UnavailableRole, // the session can't judge it to ON; why says why
        WhyRole,         // whyText of core's reason, "" when judged
        FiredRole,       // firings since the attach
        ValueRole,       // the settling term's sample in the display unit,
                         // NaN before one
        UnitRole,        // its display unit
        DecimalsRole,
        SinceMsRole, // session time of its last change, ms; -1 before
        DefIdsRole   // for each term, the def id it binds, -1 for none
    };
    Q_ENUM(Role)

    explicit ConditionModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return static_cast<int>(m_rows.size()); }
    // Some enabled condition has record: what an armed recorder needs
    bool anyRecords() const;

    // The rows of set (nullptr: none), each term bound to signal_set and
    // file as a session binds them; a NULL signal_set binds nothing and
    // the rows aren't live. The rows copy what they show.
    void setConditions(const ecuio_condition_set_t* set,
                       const ecuio_signal_set_t* signal_set,
                       const ecuio_signal_file_t* file);
    // The session's states, one per row in order; the rows read live
    void applyStates(const ecuio_condition_state_t* states, uint32_t count);
    // The term an event of a row's was settled by: its value's unit
    void applyTerm(int row, uint32_t term);
    // Unjudged again: the session ended
    void clearStates();
    // ECUIO_UNITS_*; the value roles of every row are notified
    void setUnitSystem(uint32_t units);

    // A firing's activity line: "Alert: <name>, <value> <unit>" for a
    // condition with an alert, "Condition: <name>, ..." for one without
    QString firingText(int row, double value, uint32_t term) const;
    // "Alert: <name> fired N times", or "Condition: ..."
    QString firingsText(int row, uint64_t firings) const;
    // For each def an alert ON reads, its highest level (ECUIO_ALERT_*)
    std::map<uint32_t, int> alertLevels() const;
    // A row's key, "" past the rows; the row of a key, -1 for none
    QString key(int row) const;
    int rowOfKey(const QString& key) const;
    // A row's level when its alert sounds, ECUIO_ALERT_NONE when not
    uint32_t toneLevel(int row) const;
    // Whether any row's alert sounds
    bool sounds() const;

    // Core's reason (ECUIO_CONDITION_WHY_*) in the app's words
    static QString whyText(uint32_t why);

signals:
    void countChanged();

private:
    struct Term {
        uint32_t defId = UINT32_MAX; // the def it binds, UINT32_MAX none
        QByteArray unit;             // that def's
        uint8_t decimals = 0;
    };
    struct Row {
        QString key;
        QString name;
        uint32_t flags = 0;
        uint32_t uses = 0;
        uint32_t level = ECUIO_ALERT_NONE;
        std::vector<Term> terms;
        uint32_t valueTerm = 0; // the term the value is read in
        bool live = false;
        ecuio_condition_state_t state{};
    };
    // A value as shown, in the display unit
    ecuio_display_value_t shown(const Row& r, double value,
                                uint32_t term) const;
    QString prefix(const Row& r) const;

    std::vector<Row> m_rows;
    uint32_t m_units = ECUIO_UNITS_METRIC;
};
