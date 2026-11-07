#pragma once

#include <memory>

#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>

#include <vlc/vlc.h>

namespace ncm {

class Bridge;
class NativeNetwork;
class StreamCache;
struct RuntimePaths;

class AudioService final : public QObject
{
public:
    AudioService(Bridge &, NativeNetwork &, const RuntimePaths &, QObject *parent = nullptr);
    ~AudioService() override;

private:
    void registerHandlers();

    void load(const QString &id, const QJsonObject &info);
    void play(const QString &command);
    void pause(const QString &command);
    void stop();
    void end(int code);
    void poll();
    void acknowledgeState();

    void setVolume(double);
    void fadeTo(double factor);
    void applyVolume();

    Bridge &bridge_;
    libvlc_instance_t *instance_ = nullptr;
    libvlc_media_player_t *player_ = nullptr;
    libvlc_media_t *media_ = nullptr;
    std::unique_ptr<StreamCache> cache_;
    QUrl cachedSource_;
    QTimer poll_;
    QVariantAnimation fade_;
    QString cacheRoot_;
    QString settingsPath_;

    QString id_;
    QString path_;
    QString command_;
    double offset_ = 0;
    double duration_ = 0;
    double buffered_ = 0;
    double volume_ = 1;
    double fadeFactor_ = 1;
    double reportedPosition_ = -1;
    double reportedBuffered_ = -1;
    int buffering_ = -1;
    bool segment_ = false;
    bool loaded_ = false;
    bool ended_ = false;
    bool wantPlay_ = false;
    bool statePending_ = false;
    bool fadeEnabled_ = false;
};

}
