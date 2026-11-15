// File/layer: app/documentpicker.cpp | Qt6 Core/Qml (+ CorePrivate on
//             Android), app layer, C++17
// Testing: QML: QmlTest.documentpicker (test_qml); MANUAL SMOKE: the Android
//          run sheet's picker checks

#include "documentpicker.h"
#include "urlwrite.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <QThreadPool>
#include <QtCore/qcoreapplication_platform.h>
// Private API: a Qt that changes it breaks CI's android-build, and the way
// out is an activity of our own handing the picker's result to native code
#include <QtCore/private/qjnihelpers_p.h>
#endif

namespace {
// Every code a pick gets is the listener's, 256 picks in turn get 256 codes,
// and the listener takes no code past them
constexpr bool codesAreTheListeners()
{
    bool seen[256] = {};
    for (int request = 0; request < 0x400; ++request) {
        const int code = DocumentPicker::codeOf(request);
        if (!DocumentPicker::ownsRequestCode(code)) {
            return false;
        }
        if (request < 256) {
            const int slot = code - DocumentPicker::kRequestCodeBase;
            if (seen[slot]) {
                return false;
            }
            seen[slot] = true;
        }
    }
    return !DocumentPicker::ownsRequestCode(DocumentPicker::kRequestCodeBase -
                                            1) &&
           !DocumentPicker::ownsRequestCode(DocumentPicker::kRequestCodeBase +
                                            0x100);
}
static_assert(codesAreTheListeners(),
              "the listener takes exactly the codes the picks get");
const char* verdictWords(PickMade made)
{
    switch (made) {
    case PickMade::Made:
        return "made by the pick";
    case PickMade::Replaced:
        return "chosen to replace";
    case PickMade::Unknown:
        break;
    }
    return "no time, judged by its bytes";
}
} // namespace

int DocumentPicker::requestOfCode(int code) const
{
    return requestCodeOf(m_pending) == code ? m_pending : 0;
}

int DocumentPicker::pick()
{
    const int request = ++m_last;
    m_pending = request;
    m_save = false;
    start(request);
    return request;
}

int DocumentPicker::pickSave(const QString& title, const QString& suffix)
{
    const int request = ++m_last;
    m_pending = request;
    m_save = true;
    m_title = title;
    m_mimeType = mimeTypeOf(suffix);
    m_launchedMs = QDateTime::currentMSecsSinceEpoch();
    start(request);
    return request;
}

QString DocumentPicker::mimeTypeOf(const QString& suffix) const
{
    const QString s = suffix.toLower();
    if (s == QLatin1String("json")) {
        return QStringLiteral("application/json");
    }
    if (s == QLatin1String("csv")) {
        return QStringLiteral("text/csv");
    }
    if (s == QLatin1String("txt")) {
        return QStringLiteral("text/plain");
    }
    return QStringLiteral("application/octet-stream");
}

void DocumentPicker::deliver(int request, int resultCode, const QString& uri,
                             const QString& name, qint64 modifiedMs)
{
    if (request == 0 || request != m_pending) {
        return;
    }
    m_pending = 0;
    if (resultCode != kResultOk) {
        emit canceled(request);
    } else if (uri.isEmpty()) {
        emit failed(request, tr("The picker returned no document"));
    } else {
        if (m_save) {
            // Noted before the answer, so the save it starts finds it, under
            // the name that save opens it by
            const qint64 answeredMs = QDateTime::currentMSecsSinceEpoch();
            const PickMade made =
                judgePick(m_launchedMs, answeredMs, modifiedMs);
            notePick(pathOfUrl(pickedUrl(uri)), made);
            // No URI or name: a cloud path can carry a person's folders
            qInfo("DocumentPicker: a save's document, last modified %lld, "
                  "the picker launched %lld, answered %lld: %s",
                  static_cast<long long>(modifiedMs),
                  static_cast<long long>(m_launchedMs),
                  static_cast<long long>(answeredMs), verdictWords(made));
        }
        emit picked(request, pickedUrl(uri), name);
    }
}

int DocumentPicker::verdictOfForTesting(const QString& uri) const
{
    return static_cast<int>(peekPick(pathOfUrl(pickedUrl(uri))));
}

// Queued, so the caller has the request number before its answer
void DocumentPicker::failLater(int request, const QString& words)
{
    QMetaObject::invokeMethod(
        this,
        [this, request, words] {
            if (request != m_pending) {
                return;
            }
            m_pending = 0;
            emit failed(request, words);
        },
        Qt::QueuedConnection);
}

#ifdef Q_OS_ANDROID
namespace {

// A QJniObject call clears a Java exception before it returns, so each
// answer here is judged by what came back: an invalid object, or false

QJniObject javaText(const char* text)
{
    return QJniObject::fromString(QString::fromLatin1(text));
}

struct DocumentFacts {
    QString name;
    qint64 modifiedMs = -1;
};

// OpenableColumns.DISPLAY_NAME of the picked document ("" when the provider
// doesn't say) and, asked for, its Document.COLUMN_LAST_MODIFIED (-1 when it
// doesn't)
DocumentFacts documentFacts(const QJniObject& uri, bool withModified)
{
    DocumentFacts facts;
    if (!uri.isValid()) {
        return facts;
    }
    QJniEnvironment env;
    // Plain JNI, checked: a null projection asks for every column
    const jclass text = env.findClass("java/lang/String");
    jobjectArray columns =
        text == nullptr
            ? nullptr
            : env->NewObjectArray(withModified ? 2 : 1, text,
                                  javaText("_display_name").object());
    if (columns != nullptr && withModified) {
        env->SetObjectArrayElement(columns, 1,
                                   javaText("last_modified").object());
    }
    if (columns == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (columns != nullptr) {
            env->DeleteLocalRef(columns);
        }
        return facts;
    }
    const QJniObject cursor = contentResolver().callObjectMethod(
        "query",
        "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;"
        "[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
        uri.object(), columns, static_cast<jobject>(nullptr),
        static_cast<jobject>(nullptr), static_cast<jobject>(nullptr));
    env->DeleteLocalRef(columns);
    if (cursor.isValid()) {
        // By name: a provider may answer more columns than were asked
        const auto columnOf = [&cursor](const char* name) {
            return cursor.callMethod<jint>("getColumnIndex",
                                           "(Ljava/lang/String;)I",
                                           javaText(name).object());
        };
        const jint nameColumn = columnOf("_display_name");
        const jint modifiedColumn =
            withModified ? columnOf("last_modified") : -1;
        if (cursor.callMethod<jboolean>("moveToFirst")) {
            if (nameColumn >= 0) {
                facts.name =
                    cursor
                        .callObjectMethod("getString", "(I)Ljava/lang/String;",
                                          nameColumn)
                        .toString();
            }
            if (modifiedColumn >= 0 && !cursor.callMethod<jboolean>(
                                           "isNull", "(I)Z", modifiedColumn)) {
                facts.modifiedMs =
                    cursor.callMethod<jlong>("getLong", "(I)J", modifiedColumn);
            }
        }
        cursor.callMethod<void>("close");
    }
    return facts;
}

// On the GUI thread: the result reaches the pick it answers, the name asked
// on a pool thread first, since a cloud provider's query can take seconds
void answer(const QPointer<DocumentPicker>& self, int code, int resultCode,
            const QString& uri)
{
    const int request = self ? self->requestOfCode(code) : 0;
    if (request == 0) {
        return;
    }
    if (resultCode != DocumentPicker::kResultOk || uri.isEmpty()) {
        self->deliver(request, resultCode, uri, {});
        return;
    }
    const bool save = self->savePending();
    QThreadPool::globalInstance()->start(
        [self, request, resultCode, uri, save] {
            const DocumentFacts facts = documentFacts(contentUri(uri), save);
            QCoreApplication* app = QCoreApplication::instance();
            if (app == nullptr) {
                return;
            }
            QMetaObject::invokeMethod(
                app,
                [self, request, resultCode, uri, facts] {
                    if (self) {
                        self->deliver(request, resultCode, uri, facts.name,
                                      facts.modifiedMs);
                    }
                },
                Qt::QueuedConnection);
        });
}

} // namespace

// Called on Android's main thread: only the URI is read there
struct DocumentPicker::ResultListener
    : QtAndroidPrivate::ActivityResultListener {
    explicit ResultListener(DocumentPicker* picker) : self(picker) {}

    bool handleActivityResult(jint requestCode, jint resultCode,
                              jobject data) override
    {
        if (!ownsRequestCode(requestCode)) {
            return false;
        }
        QString uri;
        if (resultCode == kResultOk && data != nullptr) {
            const QJniObject picked = QJniObject(data).callObjectMethod(
                "getData", "()Landroid/net/Uri;");
            if (picked.isValid()) {
                uri = picked.toString();
            }
        }
        QCoreApplication* app = QCoreApplication::instance();
        if (app != nullptr) {
            QMetaObject::invokeMethod(
                app,
                [picker = self, requestCode, resultCode, uri] {
                    answer(picker, requestCode, resultCode, uri);
                },
                Qt::QueuedConnection);
        }
        return true;
    }

    QPointer<DocumentPicker> self;
};

DocumentPicker::DocumentPicker(QObject* parent)
    : QObject(parent), m_listener(std::make_unique<ResultListener>(this))
{
    QtAndroidPrivate::registerActivityResultListener(m_listener.get());
}

// Qt's dispatch holds the listeners' lock this takes (qtbase's source), so
// no result is mid-call
DocumentPicker::~DocumentPicker()
{
    QtAndroidPrivate::unregisterActivityResultListener(m_listener.get());
}

bool DocumentPicker::available() const
{
    return true;
}

void DocumentPicker::start(int request)
{
    QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V",
                      javaText(m_save ? "android.intent.action.CREATE_DOCUMENT"
                                      : "android.intent.action.OPEN_DOCUMENT")
                          .object());
    intent.callObjectMethod(
        "addCategory", "(Ljava/lang/String;)Landroid/content/Intent;",
        javaText("android.intent.category.OPENABLE").object());
    // An open asks for every file: each dialog offers All files, and a
    // provider's MIME type for a .json or .csv varies, so a filter would gray
    // out the user's file. A save names its own type
    const QJniObject type =
        m_save ? QJniObject::fromString(m_mimeType) : javaText("*/*");
    intent.callObjectMethod("setType",
                            "(Ljava/lang/String;)Landroid/content/Intent;",
                            type.object());
    if (m_save && !m_title.isEmpty()) {
        intent.callObjectMethod(
            "putExtra",
            "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
            javaText("android.intent.extra.TITLE").object(),
            QJniObject::fromString(m_title).object());
    }
    // Asked first for words of its own (the manifest's <queries> makes the
    // picker visible)
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject handler = intent.callObjectMethod(
        "resolveActivity",
        "(Landroid/content/pm/PackageManager;)Landroid/content/ComponentName;",
        context
            .callObjectMethod("getPackageManager",
                              "()Landroid/content/pm/PackageManager;")
            .object());
    if (!handler.isValid()) {
        failLater(request, m_save ? tr("No app on this phone saves documents")
                                  : tr("No app on this phone opens documents"));
        return;
    }
    // Plain JNI, checked: Qt's own launch clears an exception unseen (a
    // SecurityException), and the pick would get no answer
    const QJniObject activity = QtAndroidPrivate::activity();
    QJniEnvironment env;
    jclass cls =
        activity.isValid() ? env->GetObjectClass(activity.object()) : nullptr;
    const jmethodID launch =
        cls != nullptr ? env.findMethod(cls, "startActivityForResult",
                                        "(Landroid/content/Intent;I)V")
                       : nullptr;
    if (cls != nullptr) {
        env->DeleteLocalRef(cls);
    }
    bool started = false;
    if (launch != nullptr) {
        env->CallVoidMethod(activity.object(), launch, intent.object(),
                            static_cast<jint>(requestCodeOf(request)));
        started = !env->ExceptionCheck();
    }
    if (!started) {
        // Described in the log, then cleared
        QJniEnvironment::checkAndClearExceptions(env.jniEnv());
        failLater(request, tr("Android couldn't open its document picker"));
    }
}
#else
struct DocumentPicker::ResultListener {};

DocumentPicker::DocumentPicker(QObject* parent) : QObject(parent) {}

DocumentPicker::~DocumentPicker() = default;

bool DocumentPicker::available() const
{
    return false;
}

void DocumentPicker::start(int request)
{
    if (!m_pickerForTesting) {
        failLater(request, tr("This build has no document picker"));
    }
}
#endif
