// File/layer: app/signalfilter.cpp | Qt Core, app layer, C++17
// Testing: UNIT: BridgeTest.SignalFilter*; see signalfilter.h

#include "signalfilter.h"

#include <QVariantMap>

#include "ecuio/signals.h"

SignalFilter::SignalFilter(QObject* parent) : QObject(parent) {}

void SignalFilter::setText(const QString& text)
{
    if (text == m_text) {
        return;
    }
    m_text = text;
    m_words = text.simplified().toCaseFolded().split(QLatin1Char(' '),
                                                     Qt::SkipEmptyParts);
    emit changed();
}

void SignalFilter::setPlottedOnly(bool on)
{
    if (on == m_plottedOnly) {
        return;
    }
    m_plottedOnly = on;
    emit changed();
}

QVariantList SignalFilter::groups() const
{
    QVariantList list;
    for (int g : m_groups) {
        list.append(g);
    }
    return list;
}

bool SignalFilter::active() const
{
    return !m_words.isEmpty() || m_plottedOnly || !m_groups.empty();
}

bool SignalFilter::matches(const QString& name, const QString& unit, int group,
                           bool plotted) const
{
    if (m_plottedOnly && !plotted) {
        return false;
    }
    if (!m_groups.empty() && m_groups.count(known(group)) == 0) {
        return false;
    }
    if (m_words.isEmpty()) {
        return true;
    }
    const QString n = name.toCaseFolded();
    const QString u = unit.toCaseFolded();
    for (const QString& w : m_words) {
        if (!n.contains(w) && !u.contains(w)) {
            return false;
        }
    }
    return true;
}

bool SignalFilter::chosen(int group) const
{
    return m_groups.count(known(group)) != 0;
}

void SignalFilter::toggleGroup(int group)
{
    const int g = known(group);
    if (m_groups.erase(g) == 0) {
        m_groups.insert(g);
    }
    emit changed();
}

void SignalFilter::clear()
{
    if (!active() && m_text.isEmpty()) {
        return;
    }
    m_text.clear();
    m_words.clear();
    m_plottedOnly = false;
    m_groups.clear();
    emit changed();
}

QVariantList SignalFilter::groupChoices(const QVariantList& codes) const
{
    std::set<int> present;
    for (const QVariant& c : codes) {
        present.insert(known(c.toInt()));
    }
    QVariantList list;
    for (int g : present) {
        list.append(QVariantMap{{QStringLiteral("code"), g},
                                {QStringLiteral("name"), groupName(g)}});
    }
    return list;
}

int SignalFilter::known(int group)
{
    return group < 0 || *ecuio_signal_group_name(
                            static_cast<uint32_t>(group)) == '\0'
               ? static_cast<int>(ECUIO_SIGNAL_GROUP_OTHER)
               : group;
}

QString SignalFilter::groupName(int group)
{
    return QString::fromUtf8(
        ecuio_signal_group_name(static_cast<uint32_t>(known(group))));
}
