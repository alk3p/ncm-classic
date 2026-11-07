#pragma once

#include "SqlDatabase.h"

#include <atomic>
#include <functional>
#include <memory>

#include <QObject>
#include <QPointer>
#include <QThread>

namespace ncm {

class DatabaseExecutor
{
public:
    DatabaseExecutor(const QString &path, const QString &dictionary = {});
    ~DatabaseExecutor();
    DatabaseExecutor(const DatabaseExecutor &) = delete;
    DatabaseExecutor &operator=(const DatabaseExecutor &) = delete;

    SqlResult initialize(bool offline);
    template<typename Work, typename Complete>
    void run(QObject *receiver, Work work, Complete complete);
    void execute(
        QObject *receiver,
        const QStringList &statements,
        bool atomic,
        std::function<void(SqlResult)> complete);

private:
    QObject delivery_;
    std::atomic_bool stopping_{false};
    QThread thread_;
    QObject *worker_ = nullptr;
    std::unique_ptr<SqlDatabase> database_;
};

template<typename Work, typename Complete>
void DatabaseExecutor::run(QObject *receiver, Work work, Complete complete)
{
    QMetaObject::invokeMethod(
        worker_,
        [this,
         receiver = QPointer(receiver),
         work = std::move(work),
         complete = std::move(complete)]() mutable {
            if (stopping_.load())
                return;
            auto result = work(*database_);
            if (stopping_.load())
                return;
            QMetaObject::invokeMethod(
                &delivery_,
                [receiver, result = std::move(result), complete = std::move(complete)]() mutable {
                    if (receiver)
                        complete(std::move(result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

}
