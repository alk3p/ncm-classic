#pragma once

#include <functional>
#include <memory>

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QThreadPool>

class QNetworkReply;

namespace ncm {

class Bridge;
class NativeNetwork;

class MultipartServices final : public QObject
{
    Q_OBJECT

public:
    MultipartServices(Bridge &, NativeNetwork &, QObject *parent = nullptr);
    ~MultipartServices() override;

private:
    struct ImageTask;
    using Completion = std::function<void(QJsonArray)>;

    void imageApi(
        const std::shared_ptr<ImageTask> &,
        const QString &,
        const QJsonObject &,
        std::function<void(QJsonObject)>);
    void imageLocate(const std::shared_ptr<ImageTask> &);
    void imageBytes(const std::shared_ptr<ImageTask> &, const QString &);
    void imageFinish(const std::shared_ptr<ImageTask> &, QJsonObject);
    void imageCancel(const std::shared_ptr<ImageTask> &);
    QString account() const;

    QHash<QString, std::shared_ptr<ImageTask>> images_;
    Bridge &bridge_;
    NativeNetwork &network_;
    QThreadPool io_;
    QHash<QString, QPointer<QNetworkReply>> uploads_;
};

}
