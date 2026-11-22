// File/layer: app/refusing_network.h | Qt6 Network + Qml, app layer, C++17
// Testing: QML: QmlTest.plaintext (the rule and both gates),
//          QmlTest.dashedit_plugpages (a listener the case owns sees no
//          connection from an Image or an XMLHttpRequest on the engine)
//
// The QML engine's network manager. The app loads nothing remote, so
// every request that could leave the machine (http, https, ftp) is
// answered with a refusal; qrc:, data: and a file on this machine pass
// through. A file: URL with a host would be opened by the pixmap loader as
// a UNC path (on Windows over SMB, which can send the user's credentials),
// so RefusingUrlInterceptor empties a string URL that names one and the
// manager refuses one too. That covers an Image, a Loader
// and a component; a Text's <img> is read by another route that asks
// neither, so the plain-text rule (every Text sets textFormat,
// tests/plaintext_pin.py) keeps outside text from being markup. Both gate
// the QML engine alone: core's sockets and the adapter streams never go
// through them, and Qt.openUrlExternally opens the system's browser by
// another route.

#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlApplicationEngine>
#include <QQmlNetworkAccessManagerFactory>
#include <QUrl>

#include <atomic>

// Whether a URL names another host: a file: URL with a host, or a path that
// is UNC (two leading separators) or a device or extended-length path
// ("//?/", "//./"). Win32 reads a slash and a backslash alike, and
// toLocalFile decodes a percent-encoded one, so every spelling is made a
// slash before the check. On Windows a path names this machine only with
// a drive letter ("C:/"): the NT spelling (\??\UNC\host) and every other
// form Qt never makes for a local file are refused.
inline bool url_names_another_host(const QUrl& url)
{
    if (url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) == 0 &&
        !url.host().isEmpty()) {
        return true;
    }
    if (!url.isLocalFile() && !url.scheme().isEmpty()) {
        return false;
    }
    QString path = url.isLocalFile() ? url.toLocalFile()
                                     : url.toString(QUrl::PreferLocalFile);
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
#ifdef Q_OS_WIN
    if (url.isLocalFile()) {
        return !(path.size() >= 3 && path.at(0).unicode() < 128 && path.at(0).isLetter() &&
                 path.at(1) == QLatin1Char(':') && path.at(2) == QLatin1Char('/'));
    }
#endif
    return path.startsWith(QLatin1String("//"));
}

class RefusingNetworkManager : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

    static bool stays(const QUrl& url)
    {
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("qrc") || scheme == QLatin1String("data")) {
            return true;
        }
        return scheme == QLatin1String("file") && !url_names_another_host(url);
    }

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice* body = nullptr) override
    {
        if (stays(request.url())) {
            return QNetworkAccessManager::createRequest(op, request, body);
        }
        // A scheme the manager has no backend for ends in a reply that
        // failed with ProtocolUnknownError, without opening a socket
        QNetworkRequest refused(request);
        refused.setUrl(QUrl(QStringLiteral("refused:")));
        return QNetworkAccessManager::createRequest(op, refused, nullptr);
    }
};

class RefusingNetworkFactory : public QQmlNetworkAccessManagerFactory
{
public:
    QNetworkAccessManager* create(QObject* parent) override
    {
        return new RefusingNetworkManager(parent);
    }
};

// Empties a URL that names another host (url_names_another_host)
class RefusingUrlInterceptor : public QQmlAbstractUrlInterceptor
{
public:
    static bool names_another_host(const QUrl& url)
    {
        return url_names_another_host(url);
    }

    // How many URLs it has emptied, for the tests that show an engine has it
    static int refused() { return refusedCount().load(); }

    QUrl intercept(const QUrl& url, DataType type) override
    {
        // Only a string URL (an Image's source): a QML or qmldir file passes,
        // since an install run from a share has its own qml/ folder there
        if (type != UrlString || !names_another_host(url)) {
            return url;
        }
        ++refusedCount();
        return QUrl();
    }

private:
    static std::atomic<int>& refusedCount()
    {
        static std::atomic<int> n{0};
        return n;
    }
};

// The two objects an engine must outlive, made before it and destroyed after
// it. Nothing the QML loads is remote: a name from a file drawn as markup
// could otherwise fetch a URL
struct RefusingNetworkHolder
{
    RefusingNetworkFactory factory;
    RefusingUrlInterceptor interceptor;
};

// The application's engine with both refusals installed
class RefusingEngine : private RefusingNetworkHolder, public QQmlApplicationEngine
{
public:
    RefusingEngine()
    {
        RefusingNetworkHolder& held = *this;
        setNetworkAccessManagerFactory(&held.factory);
        addUrlInterceptor(&held.interceptor);
    }
};
