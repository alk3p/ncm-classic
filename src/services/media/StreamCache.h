#pragma once

#include <memory>

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTcpServer>

namespace ncm {

class Bridge;
class NativeNetwork;
struct RuntimePaths;

class StreamCache final : public QObject
{
    Q_OBJECT

public:
    StreamCache(Bridge &, NativeNetwork &, const RuntimePaths &, QObject *parent = nullptr);
    ~StreamCache() override;

    QUrl source(const QString &, const QJsonObject &);
    void release(const QUrl &);
    double bufferedFraction(const QUrl &) const;

private:
    struct Entry;

    void accept();
    void trim();

    NativeNetwork &network_;
    Bridge &bridge_;
    QTcpServer server_;
    QString root_, settings_;
    QHash<QString, std::shared_ptr<Entry>> entries_;
    QSet<QString> writing_;
};

}
