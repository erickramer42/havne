// File/layer: app/refusing_network.h | Qt6 Network + Qml, app layer, C++17
// Testing: QML: QmlTest.dashedit_plugpages (a listener the case owns sees no
//          connection from an Image or an XMLHttpRequest on the engine)
//
// The QML engine's network manager. The app loads nothing remote, so
// every request that could leave the machine (http, https, ftp, a file on
// another host) is answered with a refusal; qrc:, data: and a file on this
// machine pass through. It gates the QML engine alone: core's sockets and
// the adapter streams never go through it, and Qt.openUrlExternally opens
// the system's browser by another route.

#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlNetworkAccessManagerFactory>
#include <QUrl>

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
