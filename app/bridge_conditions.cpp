// File/layer: app/bridge_conditions.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "bridgetext.h"
#include "healthreport.h"
#include "urlwrite.h"

#include <QByteArray>
#include <QFile>
#include <QStringConverter>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>

using namespace bridgetext;

// ---- Conditions (ADR-0063 sections 6 and 9) ----

namespace {

// "1 condition", "2 conditions"
QString conditionCountText(uint32_t count)
{
    return count == 1 ? QStringLiteral("1 condition")
                      : QStringLiteral("%1 conditions").arg(count);
}

} // namespace

void SessionBridge::onConditions(const ecuio_condition_event_t* events,
                                 uint32_t count, void* user_data)
{
    SessionBridge* self = static_cast<SessionBridge*>(user_data);
    if (events == nullptr || self == nullptr) {
        return;
    }
    if (!self->m_accepting.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(self->m_conditionsMutex);
    std::vector<ConditionSlot>& window =
        self->m_conditionStaging[self->m_activeConditions];
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_condition_event_t& e = events[i];
        // A set since replaced: its index means another condition now
        if (e.generation != self->m_stagingGeneration ||
            e.condition >= window.size()) {
            continue;
        }
        ConditionSlot& slot = window[e.condition];
        if ((e.flags & ECUIO_CONDITION_FIRED) != 0 && slot.firings++ == 0) {
            slot.first = e;
        }
        slot.newest = e;
        slot.has = true;
    }
}

void SessionBridge::prepareConditionStaging(uint32_t count, uint32_t generation)
{
    std::lock_guard<std::mutex> lock(m_conditionsMutex);
    for (std::vector<ConditionSlot>& window : m_conditionStaging) {
        window.assign(count, ConditionSlot{});
    }
    m_activeConditions = 0;
    m_stagingGeneration = generation;
}

bool SessionBridge::attachConditions(ecuio_session_t* session,
                                     const ecuio_condition_set_t* set,
                                     QString* why)
{
    if (m_conditionsGeneration != 0) {
        flushConditions(session);
    }
    size_t staged = 0;
    {
        std::lock_guard<std::mutex> lock(m_conditionsMutex);
        staged = m_conditionStaging[0].size();
    }
    // The slots first, under the generation core is about to begin: a
    // first judgment can come before the call returns
    const uint32_t count = ecuio_condition_set_count(set);
    const uint32_t next = m_conditionsGeneration + 1;
    prepareConditionStaging(count, next);
    const ecuio_signal_set_t* signalSet = ecuio_session_signal_set(session);
    const ecuio_signal_file_t* file = fileOfSet(signalSet);
    const ecuio_status_t rc =
        m_failAttaches > 0 ? (--m_failAttaches, ECUIO_ERR_OUT_OF_MEMORY)
                           : ecuio_session_set_conditions(session, set, file);
    if (rc != ECUIO_OK) {
        *why = detailOr(rc);
        prepareConditionStaging(static_cast<uint32_t>(staged),
                                m_conditionsGeneration);
        return false;
    }
    m_conditionsGeneration = next;
    m_conditionStates.assign(count, ecuio_condition_state_t{});
    // A condition whose key the new set keeps keeps its last tone: an edit
    // of another doesn't let it sound again within its 5 s
    std::vector<std::pair<QString, uint64_t>> toned;
    for (size_t i = 0; i < m_toneAtUs.size(); ++i) {
        if (m_toneAtUs[i] != NoTone) {
            toned.emplace_back(m_conditionModel->key(static_cast<int>(i)),
                               m_toneAtUs[i]);
        }
    }
    m_conditionModel->setConditions(set, signalSet, file);
    m_toneAtUs.assign(count, NoTone);
    for (const auto& [key, at] : toned) {
        const int row = m_conditionModel->rowOfKey(key);
        if (row >= 0 && static_cast<size_t>(row) < m_toneAtUs.size()) {
            m_toneAtUs[static_cast<size_t>(row)] = at;
        }
    }
    if (m_conditionModel->sounds()) {
        m_tone.prepare();
    }
    judgeAlertLevels();
    return true;
}

void SessionBridge::attachSessionConditions(ecuio_session_t* session,
                                            const QString& uri)
{
    m_conditionsGeneration = 0;
    m_conditionsPending.clear();
    m_conditionsFromRecording = false;
    m_conditionsLines.clear();
    prepareConditionStaging(0, 0);
    // ADR-0063 decision 12: a replay is judged by its recording's own copy
    QString unused;
    const ConditionSetPtr own = recordingConditions(uri, &unused);
    QString why;
    if (own != nullptr) {
        if (attachConditions(session, own.get(), &why)) {
            m_conditionsFromRecording = true;
            m_conditionsLines
                << QStringLiteral("Alerts: judging the recording's own %1")
                       .arg(conditionCountText(
                           ecuio_condition_set_count(own.get())));
            emit conditionsChanged();
            return;
        }
        unused = QStringLiteral("isn't judged (%1)").arg(why);
    }
    const ecuio_condition_set_t* saved = m_conditionStore.set();
    if (!unused.isEmpty()) {
        const QString judged = saved != nullptr
                                   ? QStringLiteral("the saved conditions")
                                   : QStringLiteral("none");
        m_conditionsLines << QStringLiteral(
                                 "Replay: the recording's conditions.json %1; "
                                 "judging %2")
                                 .arg(unused, judged);
    }
    if (saved == nullptr) {
        emit conditionsChanged();
        return;
    }
    if (attachConditions(session, saved, &why)) {
        m_conditionsLines << QStringLiteral("Alerts: judging %1")
                                 .arg(conditionCountText(
                                     ecuio_condition_set_count(saved)));
    } else {
        m_conditionsLines << QStringLiteral("Alerts not judged: %1").arg(why);
        showSavedConditions();
    }
    emit conditionsChanged();
}

QString SessionBridge::conditionsWait() const
{
    if (m_session == nullptr || !connected()) {
        return {};
    }
    if (m_conditionsFromRecording) {
        return QStringLiteral("applies when this replay is disconnected");
    }
    if (m_recorder != nullptr) {
        return QStringLiteral("applies when this recording ends");
    }
    return {};
}

void SessionBridge::loadSavedConditions()
{
    m_conditionsProblem.clear();
    if (!m_conditionStore.load(&m_conditionsProblem)) {
        m_conditionsProblem =
            QStringLiteral("The saved conditions weren't read: %1")
                .arg(m_conditionsProblem);
    }
    showSavedConditions();
}

void SessionBridge::reloadSavedConditionsForTesting()
{
    loadSavedConditions();
    emit conditionsChanged();
}

void SessionBridge::showSavedConditions()
{
    m_conditionModel->setConditions(m_conditionStore.set(), nullptr, nullptr);
    judgeAlertLevels();
}

void SessionBridge::applySavedConditions()
{
    if (m_session == nullptr || !connected()) {
        showSavedConditions();
        return;
    }
    QString why;
    const ecuio_condition_set_t* set = m_conditionStore.set();
    const uint32_t count = ecuio_condition_set_count(set);
    if (attachConditions(m_session, set, &why)) {
        emit logMessage(QStringLiteral("Alerts: judging %1")
                            .arg(conditionCountText(count)));
    } else {
        emit logMessage(QStringLiteral("Alerts not judged: %1").arg(why));
    }
}

void SessionBridge::endSessionConditions()
{
    m_conditionsGeneration = 0;
    prepareConditionStaging(0, 0);
    m_conditionStates.clear();
    m_toneAtUs.clear();
    m_conditionsPending.clear();
    m_conditionsFromRecording = false;
    m_conditionsLines.clear();
    showSavedConditions();
    emit conditionsChanged();
}

QString SessionBridge::conditionsText() const
{
    return QString::fromUtf8(m_conditionStore.bytes().isEmpty()
                                 ? m_conditionStore.refused()
                                 : m_conditionStore.bytes());
}

QVariantMap SessionBridge::saveConditions(const QString& text)
{
    QVariantMap result = m_conditionStore.save(text.toUtf8());
    if (!result.value(QStringLiteral("ok")).toBool()) {
        result.insert(QStringLiteral("pending"), QString());
        return result;
    }
    m_conditionsProblem.clear();
    const QString wait = conditionsWait();
    const QString saved =
        QStringLiteral("Alerts saved: %1")
            .arg(conditionCountText(static_cast<uint32_t>(
                result.value(QStringLiteral("conditions")).toInt())));
    emit logMessage(wait.isEmpty()
                        ? saved
                        : QStringLiteral("%1; the change %2").arg(saved, wait));
    m_conditionsPending = wait;
    if (wait.isEmpty()) {
        applySavedConditions();
    }
    result.insert(QStringLiteral("pending"), wait);
    emit conditionsChanged();
    return result;
}

QVariantMap SessionBridge::importConditions(const QUrl& url)
{
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    QString words;
    QString text;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        words = QStringLiteral("Couldn't read the file");
    } else {
        const QByteArray bytes = file.read(static_cast<qint64>(ECUIO_CONDITION_FILE_MAX_BYTES) + 1);
        if (bytes.size() > static_cast<qint64>(ECUIO_CONDITION_FILE_MAX_BYTES)) {
            words = QStringLiteral("The file is over %1 KiB, a condition file's most")
                        .arg(ECUIO_CONDITION_FILE_MAX_BYTES / 1024);
        } else {
            // Without ConvertInitialBom the decoder skips a leading BOM
            QStringDecoder utf8(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
            text = utf8(bytes);
            if (utf8.hasError()) {
                words = QStringLiteral("The file isn't UTF-8 text");
            }
        }
    }
    if (words.isEmpty()) {
        QVariantMap result = saveConditions(text);
        if (!result.value(QStringLiteral("ok")).toBool()) {
            emit logMessage(QStringLiteral("Alerts not imported: %1")
                                .arg(result.value(QStringLiteral("words")).toString()));
        }
        return result;
    }
    emit logMessage(QStringLiteral("Alerts not imported: %1").arg(words));
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("words"), words},
            {QStringLiteral("pending"), QString()}};
}

QVariantMap SessionBridge::exportConditions(const QUrl& url)
{
    // A file core refused is exported as it is, to be mended elsewhere
    const QByteArray bytes = m_conditionStore.bytes().isEmpty() ? m_conditionStore.refused()
                                                                : m_conditionStore.bytes();
    const bool removable = pickedDocumentRemovable(url);
    QString words;
    bool written = false;
    if (bytes.isEmpty()) {
        words = QStringLiteral("There are no saved alerts to export");
    } else {
        written = writeToUrl(url, bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, removable);
    }
    emit logMessage(written ? QStringLiteral("Alerts exported to a file (%1 bytes)")
                                  .arg(bytes.size())
                            : QStringLiteral("Alerts not exported: %1").arg(words));
    return {{QStringLiteral("ok"), written}, {QStringLiteral("words"), words}};
}

int SessionBridge::alertLevelOf(int defId) const
{
    return m_alertLevels.value(QString::number(defId)).toInt();
}

void SessionBridge::judgeAlertLevels()
{
    QVariantMap levels;
    for (const auto& [id, level] : m_conditionModel->alertLevels()) {
        levels.insert(QString::number(id), level);
    }
    if (levels != m_alertLevels) {
        m_alertLevels = levels;
        emit alertLevelsChanged();
    }
}

void SessionBridge::flushConditions(ecuio_session_t* session)
{
    if (session == nullptr || m_conditionsGeneration == 0) {
        return;
    }
    std::vector<ConditionSlot>* full = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_conditionsMutex);
        full = &m_conditionStaging[m_activeConditions];
        m_activeConditions ^= 1;
    }
    // The worker writes the other one now. Each firing is a line, in the
    // order they fired; several of one condition's in a window, one line
    struct Fired {
        uint64_t at;
        int row;
    };
    std::vector<Fired> fired;
    // One tone a tick, the loudest that may sound. A 5 s starts at the ask,
    // heard or not: one the player drops (stale, a failed generator) too
    const uint64_t now = ecuio_clock_us() + m_toneTimeAddedForTesting;
    uint32_t tone = ECUIO_ALERT_NONE;
    for (size_t i = 0; i < full->size(); ++i) {
        ConditionSlot& slot = (*full)[i];
        if (!slot.has) {
            continue;
        }
        m_conditionModel->applyTerm(static_cast<int>(i), slot.newest.term);
        if (slot.firings == 0) {
            continue;
        }
        fired.push_back({slot.first.timestamp_us, static_cast<int>(i)});
        const uint32_t level = m_conditionModel->toneLevel(static_cast<int>(i));
        if (level != ECUIO_ALERT_NONE && i < m_toneAtUs.size() &&
            (m_toneAtUs[i] == NoTone || now - m_toneAtUs[i] >= ToneGapUs)) {
            m_toneAtUs[i] = now;
            tone = std::max(tone, level);
        }
    }
    if (tone != ECUIO_ALERT_NONE) {
        m_tone.play(tone);
    }
    std::stable_sort(
        fired.begin(), fired.end(),
        [](const Fired& a, const Fired& b) { return a.at < b.at; });
    // A handler of the lines may attach again: they go out once the slots
    // and the model are done with
    QStringList lines;
    for (const Fired& f : fired) {
        const ConditionSlot& slot = (*full)[static_cast<size_t>(f.row)];
        lines << (slot.firings == 1
                      ? m_conditionModel->firingText(f.row, slot.first.value,
                                                     slot.first.term)
                      : m_conditionModel->firingsText(f.row, slot.firings));
    }
    for (ConditionSlot& slot : *full) {
        slot = ConditionSlot{};
    }
    // The states core keeps: UNAVAILABLE and NOT_RECEIVED are the getter's
    // alone, and can change after the attach
    uint32_t n = 0;
    const auto size = static_cast<uint32_t>(m_conditionStates.size());
    if (size > 0 &&
        ecuio_session_get_condition_states(session, m_conditionStates.data(),
                                           size, &n) == ECUIO_OK &&
        n == size &&
        m_conditionStates[0].generation == m_conditionsGeneration) {
        m_conditionModel->applyStates(m_conditionStates.data(), n);
    }
    m_conditionModel->judgeStaleSince(
        [this](uint32_t defId) { return m_signalModel->readAtMs(defId); },
        m_signalModel->wallOffsetMs());
    judgeAlertLevels();
    for (const QString& line : lines) {
        emit logMessage(line);
    }
}
