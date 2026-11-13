// File/layer: app/urlwrite.h | Qt6 Core, app layer, C++17, header-only
// Testing: BRIDGE: BridgeTest, RecordingExportTest (test_bridge); MANUAL SMOKE:
//          the Android run sheet (a provider's document written and closed)
//
// Bytes to the file a picker chose. A local file is written whole or not
// at all: QSaveFile writes beside it and renames, so a failed write leaves
// what was there. Android's document picker hands back a content:// URI,
// which openPicked opens by the content resolver, where a rename can't
// happen.

#pragma once

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QSaveFile>
#ifdef Q_OS_ANDROID
#include <unistd.h>

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#endif
#include <QString>
#include <QUrl>

// The name a picked URL is opened by: a local file's path, a provider's
// URI as text, encoded as Android's Uri.parse wants it
inline QString pathOfUrl(const QUrl& url)
{
    return url.isLocalFile() ? url.toLocalFile()
                             : url.toString(QUrl::FullyEncoded);
}

// The URL of a URI as Android's picker gives it, encoded: pathOfUrl gives the
// same text back, so the content resolver is asked for the document picked
inline QUrl pickedUrl(const QString& uri)
{
    return QUrl::fromEncoded(uri.toUtf8(), QUrl::TolerantMode);
}

#ifdef Q_OS_ANDROID
// Qt's content file engine can't open a document whose name holds
// parentheses ("report (1).csv", which the picker makes for a name taken):
// it takes the document for missing and fails to create it. A provider's
// document is opened through the content resolver and handed to QFile as a
// descriptor, which the engine never sees
inline QJniObject contentUri(const QString& where)
{
    return QJniObject::callStaticObjectMethod(
        "android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;",
        QJniObject::fromString(where).object<jstring>());
}

inline QJniObject contentResolver()
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    return context.callObjectMethod("getContentResolver",
                                    "()Landroid/content/ContentResolver;");
}

// Calls a ParcelFileDescriptor's no-argument method through plain JNI, since a
// QJniObject call clears the exception first; true when it threw or is missing
inline bool callThrows(const QJniObject& pfd, const char* name,
                       jint* got = nullptr)
{
    QJniEnvironment env;
    jclass cls = env->GetObjectClass(pfd.object());
    const jmethodID method =
        cls != nullptr
            ? env.findMethod(cls, name, got != nullptr ? "()I" : "()V")
            : nullptr;
    if (cls != nullptr) {
        env->DeleteLocalRef(cls);
    }
    if (method == nullptr) {
        env->ExceptionClear();
        return true;
    }
    if (got != nullptr) {
        *got = env->CallIntMethod(pfd.object(), method);
    } else {
        env->CallVoidMethod(pfd.object(), method);
    }
    const bool threw = env->ExceptionCheck();
    if (threw) {
        env->ExceptionClear();
    }
    return threw;
}

// Closes a ParcelFileDescriptor with closeWithError, so a provider that
// watches its pipe is told the write failed; plain JNI, as callThrows. Closed
// plainly when that call can't be made
inline void closeWithError(const QJniObject& pfd)
{
    QJniEnvironment env;
    jclass cls = env->GetObjectClass(pfd.object());
    const jmethodID method =
        cls != nullptr
            ? env.findMethod(cls, "closeWithError", "(Ljava/lang/String;)V")
            : nullptr;
    if (cls != nullptr) {
        env->DeleteLocalRef(cls);
    }
    jstring words =
        method != nullptr ? env->NewStringUTF("The write failed") : nullptr;
    env->ExceptionClear();
    if (words != nullptr) {
        env->CallVoidMethod(pfd.object(), method, words);
        env->DeleteLocalRef(words);
        env->ExceptionClear();
    } else {
        callThrows(pfd, "close");
    }
}
#endif

// A file a picker chose, open for writing: on Android it also holds the
// provider's ParcelFileDescriptor, which closePicked closes, so the provider
// sees a whole close and can refuse it; a descriptor detached from it can't.
// One still held at destruction wasn't written whole: the provider is told
class PickedFile : public QFile
{
public:
    PickedFile() = default;
    PickedFile(const PickedFile&) = delete;
    PickedFile& operator=(const PickedFile&) = delete;
    ~PickedFile() override
    {
#ifdef Q_OS_ANDROID
        if (pfd.isValid()) {
            close();
            closeWithError(pfd);
        }
#endif
    }
#ifdef Q_OS_ANDROID
    QJniObject pfd;
#endif
};

#ifdef Q_OS_ANDROID
// Opens file on a dup() of the descriptor the content resolver gives for
// where; the descriptor is handed to keep, or closed at once without one
inline bool openThroughResolver(QFile& file, const QString& where,
                                QIODevice::OpenMode mode, QJniObject* keep)
{
    Q_ASSERT(mode == QIODevice::ReadOnly ||
             mode == (QIODevice::WriteOnly | QIODevice::Truncate));
    const char* access = (mode & QIODevice::WriteOnly) ? "wt" : "r";
    QJniObject pfd = contentResolver().callObjectMethod(
        "openFileDescriptor",
        "(Landroid/net/Uri;Ljava/lang/String;)"
        "Landroid/os/ParcelFileDescriptor;",
        contentUri(where).object(),
        QJniObject::fromString(QLatin1String(access)).object<jstring>());
    if (!pfd.isValid()) {
        return false;
    }
    jint held = -1;
    const int fd =
        callThrows(pfd, "getFd", &held) || held < 0 ? -1 : ::dup(held);
    const bool opened = fd >= 0 && file.open(fd, mode, QFile::AutoCloseHandle);
    if (fd >= 0 && !opened) {
        ::close(fd);
    }
    if (opened && keep != nullptr) {
        *keep = pfd;
    } else {
        pfd.callMethod<void>("close");
        QJniEnvironment().checkAndClearExceptions();
    }
    return opened;
}
#endif

// Opens file on where, a path or a provider's URI: false when it can't be
// opened, which for a document that doesn't exist is also the answer. A
// provider's document opens here for reading alone; a write is a PickedFile's
inline bool openPicked(QFile& file, const QString& where,
                       QIODevice::OpenMode mode)
{
#ifdef Q_OS_ANDROID
    if (where.startsWith(QLatin1String("content://"))) {
        Q_ASSERT(mode == QIODevice::ReadOnly);
        return openThroughResolver(file, where, mode, nullptr);
    }
#endif
    file.setFileName(where);
    return file.open(mode);
}

// Opens a picked file for reading or for WriteOnly|Truncate, a provider's
// descriptor kept until closePicked
inline bool openPicked(PickedFile& file, const QString& where,
                       QIODevice::OpenMode mode)
{
#ifdef Q_OS_ANDROID
    if (where.startsWith(QLatin1String("content://"))) {
        return openThroughResolver(file, where, mode, &file.pfd);
    }
#endif
    file.setFileName(where);
    return file.open(mode);
}

// Removes the document at where, a path or a provider's URI
inline bool removePicked(const QString& where)
{
#ifdef Q_OS_ANDROID
    if (where.startsWith(QLatin1String("content://"))) {
        const bool gone = QJniObject::callStaticMethod<jboolean>(
            "android/provider/DocumentsContract", "deleteDocument",
            "(Landroid/content/ContentResolver;Landroid/net/Uri;)Z",
            contentResolver().object(), contentUri(where).object());
        return !QJniEnvironment().checkAndClearExceptions() && gone;
    }
#endif
    return QFile::remove(where);
}

// Closes a picked file and says whether the write held. On Android the pfd
// closes last (the dup's close leaves its descriptor open): its close reads
// the provider's status once, without waiting, and checkError after it throws
// an error the provider sent by then. One sent only after the provider sees
// the end of the document can't reach the app
inline bool closePicked(PickedFile& file)
{
    file.close();
    bool held = file.error() == QFileDevice::NoError;
#ifdef Q_OS_ANDROID
    if (file.pfd.isValid()) {
        // Whether this provider can report an error at all, for the log
        qInfo("closePicked: the provider %s report an error",
              file.pfd.callMethod<jboolean>("canDetectErrors") ? "can"
                                                               : "can't");
        if (held) {
            held = !callThrows(file.pfd, "close");
        } else {
            closeWithError(file.pfd);
        }
        held = !callThrows(file.pfd, "checkError") && held;
        file.pfd = QJniObject();
    }
#endif
    return held;
}

inline bool writeToUrl(const QUrl& url, const QByteArray& bytes)
{
    if (url.isLocalFile()) {
        QSaveFile out(url.toLocalFile());
        return out.open(QIODevice::WriteOnly) &&
               out.write(bytes) == bytes.size() && out.commit();
    }
    PickedFile out;
    return openPicked(out, pathOfUrl(url),
                      QIODevice::WriteOnly | QIODevice::Truncate) &&
           out.write(bytes) == bytes.size() && out.flush() && closePicked(out);
}
