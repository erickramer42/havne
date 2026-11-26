// File/layer: app/signalfilter.cpp | Qt Core, app layer, C++17
// Testing: UNIT: BridgeTest.SignalFilter*; see signalfilter.h

#include "signalfilter.h"

#include <QVariantMap>

#include <map>

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

void SignalFilter::setAskedOnly(bool on)
{
    if (on == m_askedOnly) {
        return;
    }
    m_askedOnly = on;
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

QVariantList SignalFilter::modules() const
{
    QVariantList list;
    for (int m : m_modules) {
        list.append(m);
    }
    return list;
}

bool SignalFilter::active() const
{
    return !m_words.isEmpty() || m_plottedOnly || m_askedOnly ||
           !m_groups.empty() || !m_modules.empty();
}

bool SignalFilter::matches(const QString& name, const QString& unit, int group,
                           bool plotted, bool asked, int module) const
{
    if (!m_modules.empty() && m_modules.count(module) == 0) {
        return false;
    }
    if (m_plottedOnly && !plotted) {
        return false;
    }
    if (m_askedOnly && !asked) {
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

bool SignalFilter::moduleChosen(int module) const
{
    return m_modules.count(module) != 0;
}

void SignalFilter::toggleModule(int module)
{
    if (module < 0) {
        return;
    }
    if (m_modules.erase(module) == 0) {
        m_modules.insert(module);
    }
    emit changed();
}

QVariantList SignalFilter::moduleChoices(const QVariantList& rows) const
{
    std::map<int, QString> present;
    for (const QVariant& r : rows) {
        const QVariantMap row = r.toMap();
        const int m = row.value(QStringLiteral("module"), -1).toInt();
        if (m >= 0) {
            present.emplace(m,
                            row.value(QStringLiteral("moduleName")).toString());
        }
    }
    QVariantList list;
    for (const auto& p : present) {
        list.append(QVariantMap{{QStringLiteral("index"), p.first},
                                {QStringLiteral("name"), p.second}});
    }
    return list;
}

void SignalFilter::clear()
{
    if (!active() && m_text.isEmpty()) {
        return;
    }
    m_text.clear();
    m_words.clear();
    m_plottedOnly = false;
    m_askedOnly = false;
    m_groups.clear();
    m_modules.clear();
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
