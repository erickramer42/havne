// File/layer: app/refusing_network.h | Qt6 Network + Qml, app layer, C++17
// Testing: QML: QmlTest.dashedit_plugpages (a listener the case owns sees no
//          connection from an Image or an XMLHttpRequest on the engine)
//
// The QML engine's network manager. The app loads nothing remote, so
// every request that could leave the machine (http, https, ftp) is
// answered with a refusal; qrc:, data: and a file on this machine pass
// through. A file: URL with a host never reaches the manager: the pixmap
// loader opens it as a UNC path (on Windows over SMB, which can send the
// user's credentials), so RefusingUrlInterceptor empties it, and any UNC or
// extended-length path, first. Both gate the QML engine alone: core's sockets and the
// adapter streams never go through them, and Qt.openUrlExternally opens the
// system's browser by another route.

#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlNetworkAccessManagerFactory>
#include <QUrl>

#include <atomic>

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
        return scheme == QLatin1String("file") && url.host().isEmpty();
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

// Empties a URL that names another host: a file: URL with a host, or a
// path that is UNC (two leading slashes or backslashes) or an extended-length path
class RefusingUrlInterceptor : public QQmlAbstractUrlInterceptor
{
public:
    static bool names_another_host(const QUrl& url)
    {
        if (url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) ==
                0 &&
            !url.host().isEmpty()) {
            return true;
        }
        const QString path = url.isLocalFile() || url.scheme().isEmpty()
                                 ? url.toString(QUrl::PreferLocalFile)
                                 : QString();
        return path.startsWith(QLatin1String("\\\\")) ||
               path.startsWith(QLatin1String("//"));
    }

    // How many URLs it has emptied, for the tests that show an engine has it
    static int refused() { return refusedCount().load(); }

    QUrl intercept(const QUrl& url, DataType) override
    {
        if (!names_another_host(url)) {
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
