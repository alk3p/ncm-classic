#pragma once

#include "DatabaseExecutor.h"
#include "Profile.h"
#include "SettingsStore.h"
#include "services/library/MusicLibrary.h"

#include <atomic>

#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QThreadPool>

namespace ncm {

class Bridge;

class PersistenceService final : public QObject
{
public:
    PersistenceService(Bridge &, const RuntimePaths &, QObject *parent = nullptr);
    ~PersistenceService() override;

    bool containsMusicFile(const QString &path) const;

private:
    void registerHandlers();
    void refreshDownloads();
    QString downloadPath() const;
    QString cachePath() const;
    QString resolveFile(const QString &type, const QString &path) const;
    QJsonArray cacheTracks(const QJsonObject &match = {}) const;

    Bridge &bridge_;
    Profile profile_;
    SettingsStore settings_;
    DatabaseExecutor online_;
    QSet<QString> knownDownloads_;
    MusicLibrary library_;
    QThreadPool pool_;
    std::atomic_bool stopping_{false};
};

}
