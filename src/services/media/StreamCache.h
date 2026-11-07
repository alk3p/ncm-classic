#pragma once

#include <memory>

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTcpServer>
#include <QUrl>

class QTcpSocket;

namespace ncm {

class Bridge;
class NativeNetwork;
struct RuntimePaths;

class StreamCache final : public QObject
{
public:
    StreamCache(Bridge &, NativeNetwork &, const RuntimePaths &, QObject *parent = nullptr);
    ~StreamCache() override;

    QUrl playerPage() const;
    QUrl stream(const QString &url, const QJsonObject &info);
    QUrl file(const QString &path);
    void release(const QUrl &);
    QString cached(const QJsonObject &info) const;

private:
    struct Request;
    struct Source;
    struct Transfer;

    QUrl endpoint(const QString &name) const;
    QUrl add(std::shared_ptr<Source>);
    void drop(const QString &token);

    void accept();
    void serve(QTcpSocket *, const QByteArray &head);
    void relay(QTcpSocket *, const std::shared_ptr<Source> &, const Request &);
    void forward(const std::shared_ptr<Transfer> &);
    bool begin(Transfer &);
    void finish(const std::shared_ptr<Transfer> &);
    void stop(Transfer &);
    void commit(const Source &, qint64 size);
    bool chksum(const Source &);
    void trim();

    QString cacheRoot() const;
    void setActive(const QString &path, bool active);

    Bridge &bridge_;
    NativeNetwork &network_;
    QString defaultRoot_;
    QString settingsFile_;
    QTcpServer server_;
    QHash<QString, std::shared_ptr<Source>> sources_;
    QSet<QString> writing_;
};

}
