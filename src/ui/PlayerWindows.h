#pragma once

#include "DesktopLyrics.h"
#include "MiniPlayer.h"
#include "PlaybackClock.h"
#include "VolumeWindow.h"
#include "app/RuntimePaths.h"
#include "services/persistence/SettingsStore.h"

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QNetworkReply;

namespace ncm {

class Bridge;
class ResourceHandler;
class Skin;

class PlayerWindows final : public QObject
{
public:
    PlayerWindows(
        Bridge &, const RuntimePaths &, ResourceHandler &, Skin &, QObject *parent = nullptr);
    ~PlayerWindows() override;

    QWidget *window(const QString &name);
    void setVisible(const QString &name, bool visible);

private:
    void registerQueue();
    void registerMiniPlayer();
    void registerLyrics();
    void connectWindows();
    void publish(const QString &name, const QJsonArray &args = {});
    void setPlaying(bool);
    void showLyricsMessage(const QString &);
    void loadCover(const QUrl &);
    void tick();
    void restoreGeometry(QWidget *, const QString &group);
    void saveGeometry(QWidget *, const QString &group);

    Bridge &bridge_;
    RuntimePaths paths_;
    ResourceHandler &resources_;
    SettingsStore settings_;
    MiniPlayer mini_;
    DesktopLyrics lyrics_;
    VolumeWindow volume_;

    PlaybackClock clock_;
    qint64 offset_ = 0;
    QTimer ticker_;
    QString slogan_;

    QNetworkAccessManager covers_;
    QPointer<QNetworkReply> coverRequest_;
};

}
