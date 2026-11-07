#include "SingleInstance.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ncm {

namespace {

constexpr qint64 maxMessage = 1024 * 1024;
constexpr int handoverMs = 10000;

bool forward(const QString &server, const QStringList &args)
{
    QLocalSocket client;
    client.connectToServer(server);
    if (!client.waitForConnected(200))
        return false;
#ifdef Q_OS_WIN
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    client.write(
        QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact) + '\n');
    return client.waitForBytesWritten(1000);
}

}

bool SingleInstance::start(const QString &profile, const QStringList &args)
{
    const QDir dir(profile);
    const auto canonical = dir.canonicalPath().isEmpty() ? dir.absolutePath() : dir.canonicalPath();
    name_ = QString(NCM_APP_ID "-")
            + QString::fromLatin1(
                QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256)
                    .toHex()
                    .left(24));
    lock_ = std::make_unique<QLockFile>(dir.filePath("instance.lock"));
    lock_->setStaleLockTime(0);
    const QDeadlineTimer deadline(handoverMs);
    while (!lock_->tryLock(25)) {
        if (forward(name_, args))
            return false;
        if (deadline.hasExpired()) {
            qWarning("The profile is in use by an instance that does not answer.");
            return false;
        }
    }
    return true;
}

void SingleInstance::listen()
{
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_.listen(name_)) {
        QLocalServer::removeServer(name_);
        if (!server_.listen(name_))
            qWarning().noquote() << "Later launches cannot reach this instance:"
                                 << server_.errorString();
    }
    connect(qApp, &QCoreApplication::aboutToQuit, &server_, &QLocalServer::close);
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        while (auto *socket = server_.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            const auto receive = [this, socket] {
                if (!socket->canReadLine()) {
                    if (socket->bytesAvailable() > maxMessage)
                        socket->disconnectFromServer();
                    return;
                }
                const auto document = QJsonDocument::fromJson(socket->readLine());
                socket->disconnectFromServer();
                if (!document.isArray())
                    return;
                QStringList args;
                for (const auto value : document.array())
                    args << value.toString();
                emit activated(args);
            };
            connect(socket, &QLocalSocket::readyRead, this, receive);
            receive();
        }
    });
}

}
