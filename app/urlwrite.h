// File/layer: app/urlwrite.h | Qt6 Core, app layer, C++17, header-only
// Testing: BRIDGE: BridgeTest (test_bridge)
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
#endif

// Opens file on where, a path or a provider's URI: false when it can't be
// opened, which for a document that doesn't exist is also the answer. A
// content:// name supports ReadOnly and WriteOnly|Truncate alone: no caller
// reads and writes, or writes without truncating
inline bool openPicked(QFile& file, const QString& where,
                       QIODevice::OpenMode mode)
{
#ifdef Q_OS_ANDROID
    if (where.startsWith(QLatin1String("content://"))) {
        Q_ASSERT(mode == QIODevice::ReadOnly ||
                 mode == (QIODevice::WriteOnly | QIODevice::Truncate));
        const char* access = (mode & QIODevice::WriteOnly)
                                 ? ((mode & QIODevice::Truncate) ? "wt" : "w")
                                 : "r";
        QJniObject pfd = contentResolver().callObjectMethod(
            "openFileDescriptor",
            "(Landroid/net/Uri;Ljava/lang/String;)"
            "Landroid/os/ParcelFileDescriptor;",
            contentUri(where).object(),
            QJniObject::fromString(QLatin1String(access)).object<jstring>());
        if (QJniEnvironment().checkAndClearExceptions() || !pfd.isValid()) {
            return false;
        }
        const int fd = pfd.callMethod<jint>("detachFd");
        if (fd < 0) {
            return false;
        }
        if (!file.open(fd, mode, QFile::AutoCloseHandle)) {
            ::close(fd);
            return false;
        }
        return true;
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

// Closes a picked file and says whether the close held: a provider that
// reports a failed commit only at the close (a cloud-backed one) is known by
// it, and flush() alone doesn't see that
inline bool closePicked(QFile& file)
{
    file.close();
    return file.error() == QFileDevice::NoError;
}

inline bool writeToUrl(const QUrl& url, const QByteArray& bytes)
{
    if (url.isLocalFile()) {
        QSaveFile out(url.toLocalFile());
        return out.open(QIODevice::WriteOnly) &&
               out.write(bytes) == bytes.size() && out.commit();
    }
    QFile out;
    return openPicked(out, pathOfUrl(url),
                      QIODevice::WriteOnly | QIODevice::Truncate) &&
           out.write(bytes) == bytes.size() && out.flush() && closePicked(out);
}
