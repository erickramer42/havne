// File/layer: app/signalplot.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// SignalListModel's plot choices: a row's tick, the lists a vehicle keeps
// and which rows plot.

#include "signallistmodel.h"

#include "choicerows.h"

#include <QSet>

#include <algorithm>
#include <vector>

namespace {

// texts less those in taken, each once, the newest most kept: the front,
// since a save writes the rows' keys before the carried
QStringList carried(const QStringList& texts, const QSet<QString>& taken,
                    int most)
{
    QStringList out;
    QSet<QString> seen;
    for (const QString& t : texts) {
        if (!taken.contains(t) && !seen.contains(t)) {
            seen.insert(t);
            out << t;
        }
    }
    return out.mid(0, most);
}

} // namespace

void SignalListModel::toggleSubscribed(int row)
{
    if (row < 0 || row >= static_cast<int>(m_entries.size())) return;
    Entry& e = m_entries[static_cast<size_t>(row)];
    // A text row's default is unticked, so its tick is Plotted or Default
    const bool plot = !e.subscribed;
    e.choice = plot == e.plotDefault ? PlotChoice::Default
             : plot                  ? PlotChoice::Plotted
                                     : PlotChoice::Unplotted;
    judgePlotted();
    emit ticksChanged();
}

QStringList SignalListModel::plottedKeys() const
{
    QStringList keys;
    for (const Entry& e : m_entries) {
        if (e.choice == PlotChoice::Plotted && e.key[0] != 0) {
            keys << QString::fromUtf8(e.key);
        }
    }
    return keys + m_carriedPlotted;
}

std::vector<uint16_t> SignalListModel::tickedIds() const
{
    std::vector<uint16_t> ids;
    for (const Entry& e : m_entries) {
        if (e.choice == PlotChoice::Plotted) {
            ids.push_back(e.id);
        }
    }
    return ids;
}

std::vector<uint16_t> SignalListModel::plottedIds() const
{
    std::vector<uint16_t> ids;
    for (const Entry& e : m_entries) {
        if (e.subscribed && !e.hidden) {
            ids.push_back(e.id);
        }
    }
    return ids;
}

QStringList SignalListModel::unplottedKeys() const
{
    QStringList keys;
    for (const Entry& e : m_entries) {
        if (!e.isText && e.choice == PlotChoice::Unplotted && e.key[0] != 0) {
            keys << QString::fromUtf8(e.key);
        }
    }
    return keys + m_carriedUnplotted;
}

void SignalListModel::setPlotChoices(const QStringList& plotted,
                                     const QStringList& unplotted)
{
    ChoiceRows rows;
    rows.reserve(static_cast<qsizetype>(m_entries.size()));
    for (Entry& e : m_entries) {
        rows.add(QByteArray(e.key), QByteArray(e.name));
        e.choice = PlotChoice::Default;
    }
    // The plotted first, so a row in both lists ends unplotted. A row with
    // no key (core refused the keys) can't keep its text: it is carried
    QSet<QString> answered;
    for (const QString& text : plotted) {
        const int row = rows.rowOf(text.toUtf8());
        if (row >= 0) {
            Entry& e = m_entries[static_cast<size_t>(row)];
            if (e.key[0] != 0) {
                answered.insert(text);
            }
            e.choice = PlotChoice::Plotted;
        }
    }
    m_carriedPlotted = carried(plotted, answered, CarriedMax);
    answered.clear();
    for (const QString& text : unplotted) {
        const int row = rows.rowOf(text.toUtf8());
        if (row >= 0) {
            Entry& e = m_entries[static_cast<size_t>(row)];
            if (e.key[0] != 0) {
                answered.insert(text);
            }
            if (!e.isText) {
                e.choice = PlotChoice::Unplotted;
            }
        }
    }
    m_carriedUnplotted = carried(unplotted, answered, CarriedMax);
    judgePlotted();
    emit ticksChanged();
}

void SignalListModel::dropCarriedChoices()
{
    m_carriedPlotted.clear();
    m_carriedUnplotted.clear();
}

void SignalListModel::judgePlotted()
{
    // The default: every numeric row (none on a pick-only session, which
    // asks only what is ticked), or in a ranked set the
    // DefaultPlotted shown numeric rows of the lowest rank (a tie in the
    // set's order), so a row Live hides gives its place to the next. A
    // refused row never has a value to draw, so it is no default either;
    // a tick still plots it
    std::vector<int> ranked;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        const Entry& e = m_entries[row];
        if (e.plotRank != 0 && !e.isText && !e.hidden && e.refused.isEmpty()) {
            ranked.push_back(static_cast<int>(row));
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [this](int a, int b) {
        return m_entries[static_cast<size_t>(a)].plotRank
             < m_entries[static_cast<size_t>(b)].plotRank;
    });
    if (ranked.size() > static_cast<size_t>(DefaultPlotted)) {
        ranked.resize(static_cast<size_t>(DefaultPlotted));
    }
    for (Entry& e : m_entries) {
        e.plotDefault =
            !m_ranked && !m_pickOnly && !e.isText && e.refused.isEmpty();
    }
    for (int row : ranked) {
        m_entries[static_cast<size_t>(row)].plotDefault = m_ranked;
    }

    int first = -1;
    int last = -1;
    int subscribed = 0;
    int ticked = 0;
    for (size_t row = 0; row < m_entries.size(); ++row) {
        Entry& e = m_entries[row];
        // A text row's tick asks it and never plots: subscribed shows the
        // tick, and the count and chartData leave the row out
        const bool plot = e.choice == PlotChoice::Plotted ||
                          (e.choice == PlotChoice::Default && e.plotDefault);
        if (plot != e.subscribed) {
            e.subscribed = plot;
            if (first < 0) first = static_cast<int>(row);
            last = static_cast<int>(row);
        }
        subscribed += (e.subscribed && !e.isText && !e.hidden) ? 1 : 0;
        ticked += (e.subscribed && !e.hidden) ? 1 : 0;
    }
    if (first >= 0) {
        emit dataChanged(index(first), index(last), {SubscribedRole});
    }
    if (subscribed != m_subscribedCount) {
        m_subscribedCount = subscribed;
        emit subscribedCountChanged();
    }
    if (ticked != m_tickedCount) {
        m_tickedCount = ticked;
        emit tickedCountChanged();
    }
}
