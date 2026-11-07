#pragma once

#include <map>
#include <memory>

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace ncm {

class Bridge;
class NativeNetwork;
struct RuntimePaths;

class TransferService final : public QObject
{
public:
    TransferService(Bridge &, NativeNetwork &, const RuntimePaths &, QObject *parent = nullptr);
    ~TransferService() override;

private:
    struct Task;

    void start(const QJsonObject &options);
    void stop(const QString &id, bool cancel);
    void request(Task &);
    bool receive(Task &);
    void finished(Task &);
    void finish(Task &, int code);
    void detach(Task &);
    void discard(const QString &id) const;
    void save(const Task &) const;
    void report(const Task &, bool last, int type);
    QString absolute(const QString &destination) const;
    QString partialPath(const QString &target) const;
    bool targeted(const QString &target) const;

    Bridge &bridge_;
    NativeNetwork &network_;
    QString downloadRoot_, settings_, tempFiles_;
    std::map<QString, std::unique_ptr<Task>> tasks_;
};

}
