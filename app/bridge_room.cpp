#include "sessionbridge.h"

#include <QStringList>

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "bridgetext.h"
#include "default_pick.h"
#include "signalmodules.h"

namespace {

struct Holders {
    int ticks = 0;
    std::vector<std::pair<QString, int>> gauges;
    int alerts = 0;
    bool ranked = false;

    bool any() const { return ticks > 0 || !gauges.empty() || alerts > 0; }
};

QString listText(const QStringList& items)
{
    if (items.size() < 2) {
        return items.join(QString());
    }
    return items.mid(0, items.size() - 1).join(QStringLiteral(", ")) +
           QStringLiteral(" and ") + items.last();
}

QString countOf(int n, const char* one)
{
    return QStringLiteral("%1 %2%3")
        .arg(n)
        .arg(QLatin1String(one))
        .arg(n == 1 ? QString() : QStringLiteral("s"));
}

QString holderText(const Holders& h)
{
    QStringList parts;
    if (h.ticks > 0) {
        parts << countOf(h.ticks, "tick");
    }
    for (const auto& g : h.gauges) {
        parts << countOf(g.second, "gauge") + QStringLiteral(" on ") + g.first;
    }
    if (h.alerts > 0) {
        parts << countOf(h.alerts, "alert");
    }
    return parts.join(QStringLiteral(", "));
}

std::map<uint16_t, uint32_t> modulesOf(const ecuio_signal_set_t* set)
{
    std::map<uint16_t, uint32_t> of;
    for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
        of[set->defs[i].id] = ecuio_signal_set_def_module(set, &set->defs[i]);
    }
    return of;
}

// Each module's enabled alerts: a condition's signals are core's to keep asked
std::vector<int> alertModules(const ecuio_condition_set_t* cs,
                              const ecuio_signal_set_t* set,
                              const ecuio_signal_file_t* file,
                              const std::map<uint16_t, uint32_t>& of,
                              uint32_t modules)
{
    std::vector<int> per(modules, 0);
    for (uint32_t i = 0; i < ecuio_condition_set_count(cs); ++i) {
        ecuio_condition_info_t info{};
        ecuio_condition_binding_t b{};
        if (ecuio_condition_set_get(cs, i, &info) != ECUIO_OK ||
            (info.uses & ECUIO_CONDITION_USE_ALERT) == 0 ||
            ecuio_condition_set_resolve(cs, i, set, file, &b) != ECUIO_OK ||
            b.why == ECUIO_CONDITION_WHY_DISABLED) {
            continue;
        }
        std::set<uint32_t> read;
        for (uint32_t t = 0; t < b.term_count && t < ECUIO_CONDITION_MAX_TERMS;
             ++t) {
            const auto it = of.find(static_cast<uint16_t>(b.signal_id[t]));
            if (b.signal_id[t] <= UINT16_MAX && it != of.end()) {
                read.insert(it->second);
            }
        }
        for (uint32_t m : read) {
            if (m < modules) {
                ++per[m];
            }
        }
    }
    return per;
}

struct Room {
    const ecuio_signal_set_t* set;
    const ecuio_signal_file_t* file;
    const ecuio_channel_support_t* reports;
    uint32_t modules;
    bool logging;
    bool focused;
};

QVariantMap roomAnswer(QVariantMap out, const std::vector<Holders>& held,
                       const Room& room);

} // namespace

QVariantMap SessionBridge::roomHolders(const QVariantList& pages) const
{
    QVariantMap out{{QStringLiteral("budget"), 0},
                    {QStringLiteral("noRoom"), QVariantList()},
                    {QStringLiteral("freeable"), QVariantList()},
                    {QStringLiteral("logging"), m_logActive},
                    {QStringLiteral("words"), QString()}};
    if (m_session == nullptr) {
        return out;
    }
    const ecuio_signal_set_t* set = ecuio_session_signal_set(m_session);
    const ecuio_signal_file_t* file = fileOfSet(set);
    const std::map<uint16_t, uint32_t> of = modulesOf(set);
    std::vector<Holders> held(m_channelCount);
    const auto on = [&](uint16_t id) -> Holders* {
        const auto it = of.find(id);
        return it != of.end() && it->second < m_channelCount ? &held[it->second]
                                                             : nullptr;
    };
    for (uint16_t id : m_signalModel->tickedIds()) {
        if (Holders* h = on(id)) {
            ++h->ticks;
        }
    }
    for (const QVariant& p : pages) {
        const QVariantMap page = p.toMap();
        std::map<uint32_t, int> count;
        for (uint16_t id : gaugeIds(
                 set, file, page.value(QStringLiteral("cells")).toList())) {
            const auto it = of.find(id);
            if (it != of.end() && it->second < m_channelCount) {
                ++count[it->second];
            }
        }
        for (const auto& c : count) {
            held[c.first].gauges.emplace_back(
                page.value(QStringLiteral("name")).toString(), c.second);
        }
    }
    const std::vector<int> alerts =
        alertModules(m_conditionStore.set(), set, file, of, m_channelCount);
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        held[c].alerts = alerts[c];
    }
    for (uint16_t id : m_pickIds) {
        const auto it = std::find_if(
            set->defs, set->defs + set->count,
            [id](const ecuio_signal_def_t& d) { return d.id == id; });
        Holders* h = on(id);
        if (h != nullptr && it != set->defs + set->count &&
            cli::ranked_def(*it)) {
            h->ranked = true;
        }
    }
    return roomAnswer(out, held,
                      {set, file, m_supportReports, m_channelCount, m_logActive,
                       !m_pickIds.empty()});
}

namespace {

void reasonsInto(uint32_t r, const Holders& h, bool focused,
                 std::set<QString>& why)
{
    if ((r & ECUIO_MODULE_REASON_RANGE) != 0) {
        why.insert(QStringLiteral("is read for the emissions codes"));
    }
    if ((r & ECUIO_MODULE_REASON_CONVERSATION) != 0) {
        why.insert(QStringLiteral("talks to a plugin's code"));
    }
    if ((r & ECUIO_MODULE_REASON_VISIT) != 0) {
        why.insert(QStringLiteral("has its codes read"));
    }
    if (h.ranked) {
        why.insert(QStringLiteral("holds the profile's ranked signals"));
    }
    if (r == ECUIO_MODULE_REASON_READ && !h.ranked && !focused) {
        why.insert(QStringLiteral("is read for every signal of the profile"));
    }
}

QVariantMap freeableOf(uint32_t c, const QString& name, const Holders& h)
{
    QVariantList gauges;
    for (const auto& g : h.gauges) {
        gauges << QVariantMap{{QStringLiteral("page"), g.first},
                              {QStringLiteral("count"), g.second}};
    }
    return {{QStringLiteral("module"), static_cast<int>(c)},
            {QStringLiteral("name"), name},
            {QStringLiteral("ticks"), h.ticks},
            {QStringLiteral("gauges"), gauges},
            {QStringLiteral("alerts"), h.alerts}};
}

QString roomWords(const QStringList& waiting, int open, bool logging,
                  const QStringList& freeWords, const std::set<QString>& why)
{
    const bool one = waiting.size() == 1;
    QString words =
        QStringLiteral("%1 %2 read: the adapter listens to %3 modules at a "
                       "time and all %3 are in use.")
            .arg(listText(waiting),
                 one ? QStringLiteral("isn't") : QStringLiteral("aren't"))
            .arg(open);
    if (logging) {
        return words + QStringLiteral(" The log keeps reading what it "
                                      "started with: stop it to change what "
                                      "is read.");
    }
    if (!freeWords.isEmpty()) {
        return words + QStringLiteral(" To read %1, free one: %2.")
                           .arg(one ? QStringLiteral("it")
                                    : QStringLiteral("them"),
                                freeWords.join(QStringLiteral(", ")));
    }
    const QStringList reasons(why.begin(), why.end());
    return words +
           QStringLiteral(" Every module in use %1: nothing on Live frees one.")
               .arg(reasons.isEmpty()
                        ? QStringLiteral("holds signals nothing on Live picks")
                        : reasons.join(QStringLiteral(" or ")));
}

QVariantMap roomAnswer(QVariantMap out, const std::vector<Holders>& held,
                       const Room& room)
{
    QVariantList noRoom;
    QVariantList freeable;
    QStringList waiting;
    QStringList freeWords;
    std::set<QString> why;
    int open = 0;
    for (uint32_t c = 0; c < room.modules; ++c) {
        const ecuio_channel_support_t& s = room.reports[c];
        QString name = signal_modules::nameOf(room.set, room.file, c);
        if (name.isEmpty()) {
            name = QString::number(s.rx_id, 16).toUpper();
        }
        if ((s.flags & ECUIO_SUPPORT_NO_ROOM) != 0) {
            noRoom << QVariantMap{
                {QStringLiteral("module"), static_cast<int>(c)},
                {QStringLiteral("name"), name}};
            waiting << name;
            continue;
        }
        if ((s.flags & ECUIO_SUPPORT_CLOSED) != 0) {
            continue;
        }
        ++open;
        const Holders& h = held[c];
        reasonsInto(s.open_reasons, h, room.focused, why);
        if (s.open_reasons != ECUIO_MODULE_REASON_READ || h.ranked ||
            !room.focused || room.logging || !h.any()) {
            continue;
        }
        freeable << freeableOf(c, name, h);
        freeWords << name + QStringLiteral(" (") + holderText(h) +
                         QLatin1Char(')');
    }
    out.insert(QStringLiteral("budget"), open);
    out.insert(QStringLiteral("noRoom"), noRoom);
    out.insert(QStringLiteral("freeable"), freeable);
    if (!waiting.isEmpty()) {
        out.insert(QStringLiteral("words"),
                   roomWords(waiting, open, room.logging, freeWords, why));
    }
    return out;
}

} // namespace
