#pragma once

#include "StreamCache.h"

#include <optional>
#include <string_view>

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>

#include <saucer/webview.hpp>

namespace ncm {

class Bridge;
class NativeNetwork;
class WebHost;
struct RuntimePaths;

class AudioService final : public QObject
{
public:
    AudioService(
        Bridge &, NativeNetwork &, const RuntimePaths &, WebHost &, QObject *parent = nullptr);

private:
    void registerHandlers();
    void startPlayer(WebHost &);
    saucer::status receive(std::string_view message);
    void send(const QJsonArray &command);

    void load(const QString &id, const QJsonObject &info);
    void open();
    void play(const QString &command);
    void pause(const QString &command);
    void stop();
    void end(int code);
    void update(const QJsonObject &sample);
    void setBuffering(bool);

    void fadeTo(double factor);
    void applyVolume();

    Bridge &bridge_;
    StreamCache cache_;
    QTimer poll_;
    QTimer endTimer_;
    QVariantAnimation fade_;

    QUrl source_;
    QString id_;
    QString command_;
    double offset_ = 0;
    double duration_ = 0;
    double volume_ = 1;
    double fadeFactor_ = 1;
    double reportedPosition_ = -1;
    double reportedBuffered_ = -1;
    int buffering_ = -1;
    bool loaded_ = false;
    bool started_ = false;
    bool playing_ = false;
    bool wantPlay_ = false;
    bool statePending_ = false;
    bool fadeEnabled_ = false;
    bool playerReady_ = false;
    std::optional<saucer::webview> player_;
};

}
