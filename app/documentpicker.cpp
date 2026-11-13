// File/layer: app/documentpicker.cpp | Qt6 Core/Qml (+ CorePrivate on
//             Android), app layer, C++17
// Testing: QML: QmlTest.documentpicker (test_qml); BENCH for Android's picker

#include "documentpicker.h"
#include "urlwrite.h"

#include <QCoreApplication>
#include <QPointer>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
// Private API: a Qt that changes it breaks CI's android-build, and the way
// out is an activity of our own handing the picker's result to native code
#include <QtCore/private/qandroidextras_p.h>
#endif

DocumentPicker::DocumentPicker(QObject* parent) : QObject(parent) {}

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

} // namespace

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
    // Qt's call clears an ActivityNotFoundException unseen, so no answer
    // would come: asked first (the manifest's <queries> makes it visible)
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
    const QPointer<DocumentPicker> self(this);
    // On Android's main thread: the URI and name read there (a provider's
    // query too; a slow one holds that thread), the answer queued to the GUI
    QtAndroidPrivate::startActivity(
        intent, requestCodeOf(request),
        [self, request](int, int resultCode, const QJniObject& data) {
            QString uri;
            QString name;
            if (resultCode == kResultOk && data.isValid()) {
                const QJniObject picked =
                    data.callObjectMethod("getData", "()Landroid/net/Uri;");
                if (picked.isValid()) {
                    uri = picked.toString();
                    name = displayName(picked);
                }
            }
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
#else
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
