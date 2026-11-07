#include "DatabaseExecutor.h"

namespace ncm {

DatabaseExecutor::DatabaseExecutor(const QString &path, const QString &dictionary)
    : worker_(new QObject)
{
    worker_->moveToThread(&thread_);
    QObject::connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.setObjectName("SQLite:" + path);
    thread_.start();
    QMetaObject::invokeMethod(
        worker_,
        [this, path, dictionary] { database_ = std::make_unique<SqlDatabase>(path, dictionary); },
        Qt::BlockingQueuedConnection);
}

DatabaseExecutor::~DatabaseExecutor()
{
    stopping_.store(true);
    database_->interrupt();
    QMetaObject::invokeMethod(worker_, [this] { database_.reset(); }, Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
}

SqlResult DatabaseExecutor::initialize(bool offline)
{
    SqlResult result;
    QMetaObject::invokeMethod(
        worker_,
        [this, &result, offline] { result = database_->initialize(offline); },
        Qt::BlockingQueuedConnection);
    return result;
}

void DatabaseExecutor::execute(
    QObject *receiver,
    const QStringList &statements,
    bool atomic,
    std::function<void(SqlResult)> complete)
{
    run(
        receiver,
        [statements, atomic](SqlDatabase &db) { return db.execute(statements, atomic); },
        std::move(complete));
}

}
