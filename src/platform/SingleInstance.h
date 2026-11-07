#pragma once

#include <memory>

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QStringList>

namespace ncm {

class SingleInstance final : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    bool start(const QString &profile, const QStringList &args);
    void listen();

signals:
    void activated(const QStringList &args);

private:
    QString name_;
    std::unique_ptr<QLockFile> lock_;
    QLocalServer server_;
};

}
