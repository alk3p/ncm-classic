#pragma once

#include "services/persistence/DatabaseExecutor.h"
#include "services/persistence/Profile.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

namespace ncm {

class Bridge;

class MusicLibrary final : public QObject
{
public:
    MusicLibrary(Bridge &, const Profile &, QObject *parent = nullptr);
    ~MusicLibrary() override;

    bool containsFile(const QString &path) const { return knownFiles_.contains(path); }

private:
    using Cancel = std::shared_ptr<std::atomic_bool>;

    struct Snapshot
    {
        QHash<QString, QJsonObject> files;
        QHash<QString, QList<QJsonObject>> sheets;
    };

    void registerHandlers();
    QString resolveLibrary(const QString &key) const;
    void scan(const QString &key, int batchSize = 50);
    void cancel(const QString &key);
    void walk(const QString &key, const QString &root, int batchSize, const Cancel &, Snapshot);
    void accept(
        const QString &key, const QJsonArray &rows, const Cancel &, std::function<void()> done);
    void reconcile(
        const QString &key, std::optional<QSet<QString>> seen, const Cancel &, const QString &error);
    void refreshKnown();

    Bridge &bridge_;
    DatabaseExecutor db_;
    QSet<QString> knownFiles_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
    QThreadPool pool_;
    QSet<QString> observed_, dirty_;
    QHash<QString, Cancel> scans_;
    QHash<QString, QString> scanErrors_;
};

}
