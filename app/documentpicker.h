// File/layer: app/documentpicker.h | Qt6 Core/Qml, app layer, C++17
// Testing: QML: QmlTest.documentpicker (test_qml); MANUAL SMOKE: the Android
//          run sheet's picker checks
//
// Android's own document picker for the file dialogs: ACTION_OPEN_DOCUMENT
// for the open-mode ones, ACTION_CREATE_DOCUMENT for the saves. Qt Quick's
// FileDialog checks a pick through Qt's content file engine, which takes a
// document named with parentheses ("name (1).json") for missing and keeps
// the dialog's previous pick; this hands the picked content:// URI on as it
// came, for openPicked (urlwrite.h). A save pick's answer also notes whether
// the pick made its document (urlwrite.h's notePick), so a failed save
// removes only that one. Exposed to QML as the `DocumentPicker` singleton,
// which DocumentOpenDialog.qml and DocumentSaveDialog.qml wrap. Off Android
// `available` is false and the dialogs use Qt's FileDialog.

#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQmlIntegration>

#include <memory>

class DocumentPicker : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // This build asks Android's picker
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    // Android's Activity.RESULT_OK; any other code is a cancel
    static constexpr int kResultOk = -1;

    explicit DocumentPicker(QObject* parent = nullptr);
    ~DocumentPicker() override;

    bool available() const;

    // Shows the picker and returns this pick's request number. At most one
    // of picked, canceled and failed answers it, later on the GUI thread; a
    // newer pick makes an older one's answer stale, and a stale answer is
    // dropped, so a pick whose answer never comes holds nothing up
    Q_INVOKABLE int pick();

    // Shows the save picker, offering title as the name and asking for the
    // MIME type of suffix (mimeTypeOf); answered as pick() is
    Q_INVOKABLE int pickSave(const QString& title, const QString& suffix);

    // A save's MIME type by its suffix, for the picker and a provider that
    // names the document by it: json, csv, txt, else application/octet-stream
    Q_INVOKABLE QString mimeTypeOf(const QString& suffix) const;

    // The activity's request code for a pick: a result is matched to the pick
    // under way by its code, so two picks in a row never share one. 256 in
    // turn, inside the 16 bits Android allows
    Q_INVOKABLE int requestCodeOf(int request) const { return codeOf(request); }
    static constexpr int kRequestCodeBase = 0x4400;
    static constexpr int codeOf(int request)
    {
        return kRequestCodeBase + (request & 0xFF);
    }
    static constexpr bool ownsRequestCode(int code)
    {
        return code >= kRequestCodeBase && code <= kRequestCodeBase + 0xFF;
    }

    // The pick an activity result's code answers: the pick under way when the
    // code is its, else 0 (a stale or another's result)
    Q_INVOKABLE int requestOfCode(int code) const;

    // A pick's answer as Android hands it: the result code, the document's
    // URI as text ("" for none), its display name ("" when unknown) and, for
    // a save, its last_modified in ms since the epoch (-1 when unknown),
    // judged against the launch and the answer (urlwrite.h's judgePick) and
    // noted
    Q_INVOKABLE void deliver(int request, int resultCode, const QString& uri,
                             const QString& name, qint64 modifiedMs = -1);

    // Whether the pick under way is a save's, whose answer asks last_modified
    bool savePending() const { return m_pending != 0 && m_save; }

    // Off Android, a pick waits for a deliver() as the phone's does, where it
    // would fail at once
    Q_INVOKABLE void setPickerForTesting(bool on) { m_pickerForTesting = on; }

    // The verdict noted for uri (as Android gives it), untaken: 0 none, 1
    // made, 2 replaced; and the
    // last save pick's launch, in ms since the epoch
    Q_INVOKABLE int verdictOfForTesting(const QString& uri) const;
    Q_INVOKABLE double launchedMsForTesting() const
    {
        return static_cast<double>(m_launchedMs);
    }
    // The last save pick's offered name and MIME type, as Android is asked
    Q_INVOKABLE QString titleForTesting() const { return m_title; }
    Q_INVOKABLE QString mimeTypeForTesting() const { return m_mimeType; }

signals:
    void picked(int request, const QUrl& url, const QString& name);
    void canceled(int request);
    void failed(int request, const QString& words);

private:
    void start(int request);
    void failLater(int request, const QString& words);

    // Android's activity results for the picker's codes (empty elsewhere)
    struct ResultListener;
    std::unique_ptr<ResultListener> m_listener;

    int m_last = 0;
    int m_pending = 0;
    // The pick under way's kind, and a save's name, MIME type and launch
    bool m_save = false;
    QString m_title;
    QString m_mimeType;
    qint64 m_launchedMs = 0;
    bool m_pickerForTesting = false;
};
