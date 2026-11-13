// File/layer: app/documentpicker.cpp | Qt6 Core/Qml (+ CorePrivate on
//             Android), app layer, C++17
// Testing: QML: QmlTest.documentpicker (test_qml); MANUAL SMOKE: the Android
//          run sheet's picker checks

#include "documentpicker.h"
#include "urlwrite.h"

#include <QCoreApplication>
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
} // namespace

int DocumentPicker::requestOfCode(int code) const
{
    return requestCodeOf(m_pending) == code ? m_pending : 0;
}

int DocumentPicker::pick()
{
    const int request = ++m_last;
    m_pending = request;
    start(request);
    return request;
}

void DocumentPicker::deliver(int request, int resultCode, const QString& uri,
                             const QString& name)
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
        emit picked(request, pickedUrl(uri), name);
    }
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

// OpenableColumns.DISPLAY_NAME of the picked document, "" when the provider
// doesn't say
QString displayName(const QJniObject& uri)
{
    if (!uri.isValid()) {
        return {};
    }
    QJniEnvironment env;
    // Plain JNI, checked: a null projection asks for every column
    const jclass text = env.findClass("java/lang/String");
    jobjectArray columns =
        text == nullptr
            ? nullptr
            : env->NewObjectArray(1, text, javaText("_display_name").object());
    if (columns == nullptr) {
        env->ExceptionClear();
        return {};
    }
    const QJniObject cursor = contentResolver().callObjectMethod(
        "query",
        "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;"
        "[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
        uri.object(), columns, static_cast<jobject>(nullptr),
        static_cast<jobject>(nullptr), static_cast<jobject>(nullptr));
    env->DeleteLocalRef(columns);
    QString name;
    if (cursor.isValid()) {
        // By name: a provider may answer more columns than were asked
        const jint column =
            cursor.callMethod<jint>("getColumnIndex", "(Ljava/lang/String;)I",
                                    javaText("_display_name").object());
        if (column >= 0 && cursor.callMethod<jboolean>("moveToFirst")) {
            name = cursor
                       .callObjectMethod("getString", "(I)Ljava/lang/String;",
                                         column)
                       .toString();
        }
        cursor.callMethod<void>("close");
    }
    return name;
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
    QThreadPool::globalInstance()->start([self, request, resultCode, uri] {
        const QString name = displayName(contentUri(uri));
        QCoreApplication* app = QCoreApplication::instance();
        if (app == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            app,
            [self, request, resultCode, uri, name] {
                if (self) {
                    self->deliver(request, resultCode, uri, name);
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
                      javaText("android.intent.action.OPEN_DOCUMENT").object());
    intent.callObjectMethod(
        "addCategory", "(Ljava/lang/String;)Landroid/content/Intent;",
        javaText("android.intent.category.OPENABLE").object());
    // Every file: each dialog offers All files, and a provider's MIME type for
    // a .json or .csv varies, so a filter would gray out the user's file
    intent.callObjectMethod("setType",
                            "(Ljava/lang/String;)Landroid/content/Intent;",
                            javaText("*/*").object());
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
        failLater(request, tr("No app on this phone opens documents"));
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
