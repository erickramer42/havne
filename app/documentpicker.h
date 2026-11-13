// File/layer: app/documentpicker.h | Qt6 Core/Qml, app layer, C++17
// Testing: QML: QmlTest.documentpicker (test_qml); MANUAL SMOKE: the Android
//          run sheet's picker checks
//
// Android's own document picker (ACTION_OPEN_DOCUMENT) for the open-mode
// dialogs. Qt Quick's FileDialog checks a pick through Qt's content file
// engine, which takes a document named with parentheses ("name (1).json")
// for missing and keeps the dialog's previous pick; this hands the picked
// content:// URI on as it came, for openPicked (urlwrite.h) to read.
// Exposed to QML as the `DocumentPicker` singleton, which
// DocumentOpenDialog.qml wraps. Off Android `available` is false and the
// dialogs use Qt's FileDialog.

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
    // URI as text ("" for none) and its display name ("" when unknown)
    Q_INVOKABLE void deliver(int request, int resultCode, const QString& uri,
                             const QString& name);

    // Off Android, a pick waits for a deliver() as the phone's does, where it
    // would fail at once
    Q_INVOKABLE void setPickerForTesting(bool on) { m_pickerForTesting = on; }

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
    bool m_pickerForTesting = false;
};
