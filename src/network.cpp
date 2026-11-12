// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "network.h"

#include "karten-version.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QQmlNetworkAccessManagerFactory>

using namespace Qt::StringLiterals;

namespace
{
QString userAgent()
{
    // The services ask for a stable identifier that names the application.
    // Add a contact URL here once the project has a public home.
    return u"Karten/" KARTEN_VERSION_STRING " (KDE desktop maps application)"_s;
}

class IdentifyingManager : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &original, QIODevice *data) override
    {
        QNetworkRequest request(original);
        request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
        return QNetworkAccessManager::createRequest(operation, request, data);
    }
};

class Factory : public QQmlNetworkAccessManagerFactory
{
public:
    QNetworkAccessManager *create(QObject *parent) override
    {
        auto *manager = new IdentifyingManager(parent);
        manager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        return manager;
    }
};
}

QQmlNetworkAccessManagerFactory *Network::qmlFactory()
{
    return new Factory;
}

QNetworkAccessManager *Network::manager()
{
    static QNetworkAccessManager *instance = nullptr;
    if (!instance) {
        instance = new QNetworkAccessManager(QCoreApplication::instance());
        instance->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    }
    return instance;
}

QNetworkRequest Network::request(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setTransferTimeout(30000);
    return request;
}
