#include "sessionbridge.h"
#include "bridgetext.h"
#include "enhanced_codes_line.h" /* cli/: havne-cli's enhanced_codes line */

#include <QStringList>

#include <algorithm>
#include <cstring>

using namespace bridgetext;

namespace {

constexpr qint64 kStepMs = 15000;
constexpr uint8_t kExtendedOnly = 0x7F;

bool listCodes(const ecuio_module_codes_t& r)
{
    return (r.flags & ECUIO_MODULE_CODES_COMPLETE) != 0 &&
           r.list_outcome == ECUIO_DIAG_ANSWERED;
}

QString outcomeKey(const ecuio_module_codes_t& r)
{
    if ((r.flags & ECUIO_MODULE_CODES_NOT_OPENED) != 0) {
        return QStringLiteral("not_opened");
    }
    if ((r.flags & ECUIO_MODULE_CODES_COMPLETE) == 0) {
        return QStringLiteral("not_read");
    }
    if (cli::codes_answered_nothing(r) ||
        r.list_outcome == ECUIO_DIAG_UNANSWERED) {
        return QStringLiteral("no_response");
    }
    if (r.list_outcome == ECUIO_DIAG_ANSWERED) {
        return QStringLiteral("answered");
    }
    return r.list_outcome == ECUIO_DIAG_REFUSED ? QStringLiteral("refused")
                                                : QStringLiteral("not_read");
}

QString codeText(const ecuio_module_codes_t& r, uint32_t i)
{
    char text[ECUIO_DTC_UDS_TEXT_LEN + 1] = {};
    (void)ecuio_dtc_format_uds(r.codes[i].code, r.format, text, sizeof text);
    return QString::fromLatin1(text);
}

QStringList emissionsOf(const ecuio_module_codes_t& r, uint32_t i,
                        const ecuio_channel_diagnostics_t& standard)
{
    static const char* const kLists[] = {"stored", "pending", "permanent"};
    const uint32_t bits =
        ecuio_module_code_emissions_lists(&standard, r.codes[i].code, r.format);
    QStringList lists;
    for (uint32_t k = 0; k < 3; ++k) {
        if ((bits & (1u << k)) != 0) {
            lists << QString::fromLatin1(kLists[k]);
        }
    }
    return lists;
}

uint32_t heldOf(const ecuio_module_codes_t& r)
{
    return std::min<uint32_t>(r.held, ECUIO_MODULE_CODES_MAX);
}

} // namespace

QVariantMap
BridgeFormats::moduleCodesEntry(const ecuio_module_codes_t& r,
                                const ecuio_channel_diagnostics_t& standard,
                                bool sessionOffered)
{
    QVariantMap m;
    const QString outcome = outcomeKey(r);
    m[QStringLiteral("asked")] = (r.flags & ECUIO_MODULE_CODES_ASKABLE) != 0;
    m[QStringLiteral("reading")] = (r.flags & ECUIO_MODULE_CODES_READING) != 0;
    m[QStringLiteral("complete")] =
        (r.flags & ECUIO_MODULE_CODES_COMPLETE) != 0;
    m[QStringLiteral("outcome")] = outcome;
    const bool refused = outcome == QLatin1String("refused");
    m[QStringLiteral("nrc")] =
        refused ? QStringLiteral("7F 19 %1").arg(hexByte(r.list_nrc))
                : QString();
    m[QStringLiteral("extendedOnly")] = refused && r.list_nrc == kExtendedOnly;
    m[QStringLiteral("sessionOffered")] = sessionOffered;
    QStringList codes, titles, state, cls;
    QVariantList lamp, emissions;
    const uint32_t held = listCodes(r) ? heldOf(r) : 0;
    for (uint32_t i = 0; i < held; ++i) {
        const uint8_t status = r.codes[i].status;
        codes << codeText(r, i);
        titles << QString::fromLatin1(
            ecuio_dtc_title_uds(r.codes[i].code, r.format));
        state << QString::fromLatin1(ecuio_dtc_status_state(status));
        cls << QString::fromLatin1(ecuio_dtc_status_class(status));
        lamp << (ecuio_dtc_status_lamp(status) != 0);
        emissions << QVariant(emissionsOf(r, i, standard));
    }
    m[QStringLiteral("codes")] = codes;
    m[QStringLiteral("titles")] = titles;
    m[QStringLiteral("state")] = state;
    m[QStringLiteral("cls")] = cls;
    m[QStringLiteral("lamp")] = lamp;
    m[QStringLiteral("emissions")] = emissions;
    m[QStringLiteral("more")] =
        r.reported > held && held > 0 ? r.reported - held : 0;
    return m;
}

QString
BridgeFormats::moduleCodesText(const ecuio_module_codes_t& r,
                               const ecuio_channel_diagnostics_t& standard,
                               bool sessionOffered)
{
    const QVariantMap e = moduleCodesEntry(r, standard, sessionOffered);
    const QString head =
        QStringLiteral("ECU %1 enhanced codes: ").arg(hexId(r.rx_id));
    const QString outcome = e.value(QStringLiteral("outcome")).toString();
    if (outcome == QLatin1String("no_response")) {
        return head + QStringLiteral("no response");
    }
    if (outcome == QLatin1String("not_opened")) {
        return head + QStringLiteral("not opened");
    }
    if (outcome == QLatin1String("refused")) {
        QString text =
            head + QStringLiteral("refused (%1)")
                       .arg(e.value(QStringLiteral("nrc")).toString());
        if (e.value(QStringLiteral("extendedOnly")).toBool()) {
            text += QStringLiteral(
                ": the module reads its codes only in its extended session");
            if (sessionOffered) {
                text += QStringLiteral(", connect with ?session=03");
            }
        }
        return text;
    }
    const QStringList codes = e.value(QStringLiteral("codes")).toStringList();
    if (codes.isEmpty()) {
        return head + QStringLiteral("none");
    }
    const QStringList state = e.value(QStringLiteral("state")).toStringList();
    const QStringList cls = e.value(QStringLiteral("cls")).toStringList();
    const QVariantList lamp = e.value(QStringLiteral("lamp")).toList();
    const QVariantList emissions =
        e.value(QStringLiteral("emissions")).toList();
    QStringList parts;
    for (int i = 0; i < codes.size(); ++i) {
        QStringList notes;
        if (!cls[i].isEmpty()) {
            notes << cls[i].toLower();
        }
        if (!emissions[i].toStringList().isEmpty()) {
            notes << QStringLiteral("also an emissions code");
        }
        if (lamp[i].toBool()) {
            notes << QStringLiteral("warning lamp requested");
        }
        QString part = codes[i] + QLatin1Char(' ') + state[i].toLower();
        if (!notes.isEmpty()) {
            part +=
                QStringLiteral(" (%1)").arg(notes.join(QStringLiteral(", ")));
        }
        parts << part;
    }
    QString text = head + parts.join(QStringLiteral(", "));
    const uint32_t more = e.value(QStringLiteral("more")).toUInt();
    if (more > 0) {
        text += QStringLiteral(", and %1 more").arg(more);
    }
    return text;
}

void SessionBridge::readDiagnostics()
{
    readModuleCodes();
}

void SessionBridge::readModuleCodes()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        m_codesVisit.step != CodesVisit::Step::idle) {
        return;
    }
    m_codesVisit.queue.clear();
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        m_codesVisit.queue.push_back(c);
    }
    m_codesVisit.at = 0;
    m_codesVisit.not_reached = 0;
    m_codesVisit.step = m_codesVisit.queue.empty()
                            ? CodesVisit::Step::idle
                            : CodesVisit::Step::standard_start;
    advanceCodesVisit();
}

/* One module's pass at a time: its standard codes to their end where the
 * session reads them, then its enhanced codes; a step that outlasts
 * kStepMs leaves the module not reached */
void SessionBridge::advanceCodesVisit()
{
    using Step = CodesVisit::Step;
    CodesVisit& v = m_codesVisit;
    const auto next = [&v]() {
        v.step = ++v.at < v.queue.size() ? Step::standard_start : Step::idle;
    };
    while (v.step != Step::idle) {
        if (m_session == nullptr) {
            v.step = Step::idle;
            return;
        }
        const uint32_t c = v.queue[v.at];
        ecuio_channel_diagnostics_t d{};
        const bool have =
            ecuio_session_get_channel_diagnostics(m_session, c, &d) == ECUIO_OK;
        const bool late = v.clock.isValid() && v.clock.elapsed() > kStepMs;
        if (v.step == Step::standard_start) {
            if (have && (d.flags & ECUIO_DIAG_ASKED) != 0) {
                v.generation = d.generation;
                (void)ecuio_session_read_diagnostics_channel(m_session, c);
                v.clock.restart();
                v.step = Step::standard_wait;
                return;
            }
            v.step = Step::codes_start;
        } else if (v.step == Step::standard_wait) {
            if (!have ||
                ((d.flags & ECUIO_DIAG_COMPLETE) != 0 &&
                 d.generation != v.generation) ||
                late) {
                v.step = Step::codes_start;
            } else {
                return;
            }
        } else if (v.step == Step::codes_start) {
            if (ecuio_session_read_module_codes(m_session, c, 0, &v.read_id) !=
                ECUIO_OK) {
                ++v.not_reached;
                next();
            } else {
                v.clock.restart();
                v.step = Step::codes_wait;
                return;
            }
        } else {
            ecuio_module_codes_t r{};
            if ((ecuio_session_get_module_codes(m_session, c, &r) == ECUIO_OK &&
                 (r.flags & ECUIO_MODULE_CODES_COMPLETE) != 0 &&
                 r.read_id >= v.read_id)) {
                if ((r.flags & ECUIO_MODULE_CODES_NOT_OPENED) != 0) {
                    ++v.not_reached;
                }
                next();
            } else if (late) {
                ++v.not_reached;
                next();
            } else {
                return;
            }
        }
        if (v.step == Step::idle && v.not_reached > 0) {
            emit logMessage(QStringLiteral("Enhanced codes: %1 not reached")
                                .arg(v.not_reached));
        }
    }
}

/* Each module's enhanced-codes report, logged once per read that ends;
 * the page's entries are rebuilt when a report moves */
void SessionBridge::pollModuleCodes()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    bool changed = false;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_module_codes_t r{};
        if (ecuio_session_get_module_codes(m_session, c, &r) != ECUIO_OK) {
            continue;
        }
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
        if (memcmp(&r, &m_codesShown[c], sizeof(r)) != 0) {
            m_codesShown[c] = r;
            changed = true;
        }
        if ((r.flags & ECUIO_MODULE_CODES_COMPLETE) != 0 && r.read_id != 0 &&
            r.read_id != m_codesLogged[c]) {
            m_codesLogged[c] = r.read_id;
            emit logMessage(
                moduleCodesText(r, m_diagnosticsShown[c], sessionOffered()));
        }
    }
    if (changed && m_diagnosticsShownCount > 0) {
        rebuildDiagnostics();
    }
    advanceCodesVisit();
}

bool SessionBridge::sessionOffered() const
{
    const QString uri = m_currentUri.toLower();
    return (uri.startsWith(QLatin1String("elm327://")) ||
            uri.startsWith(QLatin1String("j2534://")) ||
            uri.startsWith(QLatin1String("socketcan://"))) &&
           uri.contains(QLatin1String("profile=uds-generic")) &&
           !uri.contains(QLatin1String("session="));
}
