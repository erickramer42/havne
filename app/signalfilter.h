// File/layer: app/signalfilter.h | Qt Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge); QML: QmlTest (test_qml)

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <set>

// Finding a signal among many (ADR-0050): one rule and its state, for
// Live's list (LiveSignalModel holds one) and the Recordings page's
// signal rows (the page makes its own), so both filter alike. A row
// shows when every word typed is in its name or its unit, ignoring
// case; when it's plotted, if plottedOnly; and when its group is chosen,
// or none is. QtCore only: test_bridge compiles it.
class SignalFilter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
    Q_PROPERTY(
        bool plottedOnly READ plottedOnly WRITE setPlottedOnly NOTIFY changed)
    // The chosen groups' codes (ECUIO_SIGNAL_GROUP_*), in code order
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)
    // Anything narrows the list
    Q_PROPERTY(bool active READ active NOTIFY changed)

public:
    explicit SignalFilter(QObject* parent = nullptr);

    QString text() const { return m_text; }
    void setText(const QString& text);
    bool plottedOnly() const { return m_plottedOnly; }
    void setPlottedOnly(bool on);
    QVariantList groups() const;
    bool active() const;

    Q_INVOKABLE bool matches(const QString& name, const QString& unit,
                             int group, bool plotted) const;
    Q_INVOKABLE bool chosen(int group) const;
    Q_INVOKABLE void toggleGroup(int group);
    Q_INVOKABLE void clear();
    // A choice for each group among codes: {code, name} in code order,
    // each once, a code core doesn't know counted as Other
    Q_INVOKABLE QVariantList groupChoices(const QVariantList& codes) const;

    // A code core doesn't name is Other's (ADR-0050)
    static int known(int group);
    static QString groupName(int group);

signals:
    void changed();

private:
    QString m_text;
    QStringList m_words; // the text's words, case-folded
    bool m_plottedOnly = false;
    std::set<int> m_groups;
};
