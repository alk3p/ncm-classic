#include "AudioService.h"

#include "StreamCache.h"
#include "VlcRuntime.h"
#include "app/LegacyIdentity.h"
#include "app/RuntimePaths.h"
#include "bridge/Bridge.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSettings>

namespace ncm {

namespace {

constexpr int pollMs = 100;
constexpr int fadeMs = 500;
constexpr int invalidMedia = 2;

}

AudioService::AudioService(
    Bridge &bridge, NativeNetwork &network, const RuntimePaths &paths, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , cache_(std::make_unique<StreamCache>(bridge, network, paths))
    , cacheRoot_(paths.storageCache)
    , settingsPath_(paths.settingsFile)
{
    const char *args[]
        = {"--no-video", "--no-media-library", "--no-osd", "--quiet", "--network-caching=1500"};
    instance_ = createVlcInstance(int(std::size(args)), args);
    if (instance_) {
        libvlc_set_user_agent(instance_, legacy::vlcUserAgent, legacy::vlcHttpUserAgent);
        libvlc_set_app_id(instance_, legacy::applicationId, legacy::version, legacy::applicationId);
    }

    poll_.setInterval(pollMs);
    connect(&poll_, &QTimer::timeout, this, &AudioService::poll);
    poll_.start();

    fade_.setDuration(fadeMs);
    connect(&fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &factor) {
        fadeFactor_ = factor.toDouble();
        applyVolume();
    });
    connect(&fade_, &QVariantAnimation::finished, this, [this] {
        if (wantPlay_ || !player_ || ended_)
            return;
        libvlc_media_player_set_pause(player_, 1);
        acknowledgeState();
    });

    registerHandlers();
}

AudioService::~AudioService()
{
    poll_.stop();
    stop();
    if (player_)
        libvlc_media_player_release(player_);
    if (instance_)
        libvlc_release(instance_);
}

void AudioService::registerHandlers()
{
    for (const auto name : {"audioplayer.enmeratorDevices", "audioplayer.init"})
        bridge_.addHandler(name, [](const QJsonArray &, Bridge::Reply done) { done({}); });

    const auto current = [this](const QJsonArray &a) {
        return stringArg(a, 0) == id_ && player_ && !ended_;
    };
    bridge_.addHandler("audioplayer.load", [this](const QJsonArray &a, Bridge::Reply done) {
        load(stringArg(a, 0), a.at(1).toObject());
        done({});
    });
    bridge_.addHandler("audioplayer.play", [this, current](const QJsonArray &a, Bridge::Reply done) {
        if (current(a))
            play(stringArg(a, 1));
        done({});
    });
    bridge_.addHandler("audioplayer.pause", [this, current](const QJsonArray &a, Bridge::Reply done) {
        if (current(a))
            pause(stringArg(a, 1));
        done({});
    });
    bridge_.addHandler("audioplayer.stop", [this](const QJsonArray &a, Bridge::Reply done) {
        if (stringArg(a, 0) == id_)
            stop();
        done({});
    });
    bridge_.addHandler("audioplayer.seek", [this, current](const QJsonArray &a, Bridge::Reply done) {
        if (current(a)) {
            const double t = std::clamp(numberArg(a, 2), 0., duration_ > 0 ? duration_ : 1e9);
            const int code = libvlc_media_player_is_seekable(player_) ? 0 : -1;
            if (!code)
                libvlc_media_player_set_time(player_, qint64((t + offset_) * 1000));
            bridge_.publish("audioplayer.onSeek", {id_, a.at(1), code, t});
        }
        done({});
    });
    bridge_.addHandler("audioplayer.setVolume", [this](const QJsonArray &a, Bridge::Reply done) {
        setVolume(numberArg(a, 2));
        bridge_.publish("audioplayer.onVolume", {a.at(0), a.at(1), 0, volume_});
        done({});
    });
    bridge_.addHandler("audioplayer.switchEffect", [this](const QJsonArray &a, Bridge::Reply done) {
        for (const auto value : a.at(1).toArray())
            if (const auto option = value.toObject(); option.value("name") == "fade")
                fadeEnabled_ = option.value("on").toBool();
        if (!fadeEnabled_ && fade_.state() == QAbstractAnimation::Running) {
            fade_.stop();
            fadeFactor_ = wantPlay_ ? 1 : 0;
            applyVolume();
            if (!wantPlay_ && player_ && !ended_)
                libvlc_media_player_set_pause(player_, 1);
            acknowledgeState();
        }
        done({});
    });
}

void AudioService::load(const QString &id, const QJsonObject &info)
{
    stop();
    cacheRoot_ = QSettings(settingsPath_, QSettings::IniFormat)
                     .value("ServiceStorage/cache-path", cacheRoot_)
                     .toString();
    id_ = id;
    ended_ = false;
    segment_ = info.contains("duration");
    offset_ = info.value("starttime").toVariant().toDouble();
    duration_ = info.value("duration").toVariant().toDouble();
    buffering_ = -1;
    buffered_ = 0;
    reportedPosition_ = reportedBuffered_ = -1;

    path_ = info.value("path").toString();
    if (path_.isEmpty())
        path_ = info.value("musicurl").toString();
    if (path_ == "cacheurl" || path_.isEmpty()) {
        const auto songId = info.value("songId").toVariant().toString();
        const auto md5 = info.value("md5").toString();
        const auto bitrate = info.value("bitrate").toVariant().toInt();
        path_.clear();
        for (const auto &name : QDir(cacheRoot_).entryList({"*.cache.json"}, QDir::Files)) {
            QFile file(cacheRoot_ + '/' + name);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const auto entry = QJsonDocument::fromJson(file.readAll()).object();
            if (entry.value("songId").toVariant().toString() == songId
                && entry.value("cached").toInt() == 100
                && (md5.isEmpty() || entry.value("md5") == md5)
                && (bitrate == 0 || entry.value("bitrate").toVariant().toInt() == bitrate)) {
                path_ = entry.value("path").toString();
                break;
            }
        }
    }

    if (!player_ && instance_)
        player_ = libvlc_media_player_new(instance_);
    if (!player_ || path_.isEmpty()) {
        end(invalidMedia);
        return;
    }
    const QUrl source(path_);
    if (source.scheme() == "http" || source.scheme() == "https") {
        cachedSource_ = cache_->source(path_, info);
        media_ = libvlc_media_new_location(instance_, cachedSource_.toEncoded().constData());
    } else {
        const QFileInfo file(source.isLocalFile() ? source.toLocalFile() : path_);
        bridge_.setProperty("ncm.activeMediaPaths", QStringList{file.absoluteFilePath()});
        media_ = openLocalMedia(instance_, file.absoluteFilePath());
    }
    if (!media_) {
        end(invalidMedia);
        return;
    }
    libvlc_media_add_option(media_, ":start-paused");
    if (offset_ > 0)
        libvlc_media_add_option(
            media_, QString(":start-time=%1").arg(offset_, 0, 'f', 3).toUtf8().constData());
    if (segment_ && duration_ > 0)
        libvlc_media_add_option(
            media_,
            QString(":stop-time=%1").arg(offset_ + duration_, 0, 'f', 3).toUtf8().constData());
    libvlc_media_player_set_media(player_, media_);
    applyVolume();
    if (libvlc_media_player_play(player_) != 0)
        end(invalidMedia);
}

void AudioService::play(const QString &command)
{
    wantPlay_ = true;
    command_ = command;
    statePending_ = true;
    if (loaded_)
        fadeTo(1);
    libvlc_media_player_set_pause(player_, 0);
    acknowledgeState();
}

void AudioService::pause(const QString &command)
{
    wantPlay_ = false;
    command_ = command;
    statePending_ = true;
    if (fadeEnabled_ && libvlc_media_player_is_playing(player_) && fadeFactor_ > 0) {
        fadeTo(0);
        return;
    }
    fade_.stop();
    fadeFactor_ = 0;
    applyVolume();
    libvlc_media_player_set_pause(player_, 1);
    acknowledgeState();
}

void AudioService::stop()
{
    const auto source = cachedSource_;
    cachedSource_ = QUrl();
    id_.clear();
    path_.clear();
    duration_ = 0;
    offset_ = 0;
    segment_ = false;
    loaded_ = false;
    ended_ = true;
    wantPlay_ = false;
    statePending_ = false;
    fade_.stop();
    fadeFactor_ = 0;
    bridge_.setProperty("ncm.activeMediaPaths", QStringList());
    if (player_)
        libvlc_media_player_stop(player_);
    if (media_) {
        libvlc_media_release(media_);
        media_ = nullptr;
    }
    if (!source.isEmpty())
        cache_->release(source);
}

void AudioService::end(int code)
{
    if (ended_ || id_.isEmpty())
        return;
    ended_ = true;
    path_.clear();
    wantPlay_ = false;
    statePending_ = false;
    fade_.stop();
    if (!cachedSource_.isEmpty())
        cache_->release(cachedSource_);
    cachedSource_ = QUrl();
    bridge_.setProperty("ncm.activeMediaPaths", QStringList());
    bridge_.publish(
        code == 0 ? "audioplayer.onEnd" : "audioplayer.onLoad", {id_, QJsonObject{{"code", code}}});
}

void AudioService::poll()
{
    if (!player_ || id_.isEmpty() || ended_)
        return;
    const auto state = libvlc_media_player_get_state(player_);
    if (state == libvlc_Error || state == libvlc_Ended) {
        end(state == libvlc_Error ? invalidMedia : 0);
        return;
    }
    const int buffering = state == libvlc_Opening || state == libvlc_Buffering ? 1 : 0;
    if (buffering != buffering_) {
        buffering_ = buffering;
        bridge_.publish("audioplayer.onBuffering", {id_, buffering});
    }
    if (!loaded_ && (state == libvlc_Playing || state == libvlc_Paused)) {
        if (duration_ <= 0)
            duration_ = std::max(0., libvlc_media_player_get_length(player_) / 1000. - offset_);
        loaded_ = true;
        libvlc_media_player_set_pause(player_, wantPlay_ ? 0 : 1);
        if (wantPlay_)
            fadeTo(1);
        bridge_
            .publish("audioplayer.onLoad", {id_, QJsonObject{{"code", 0}, {"duration", duration_}}});
    }
    acknowledgeState();
    if (!loaded_)
        return;

    buffered_ = cachedSource_.isEmpty() ? 1 : cache_->bufferedFraction(cachedSource_);
    const auto position = std::max(0., libvlc_media_player_get_time(player_) / 1000. - offset_);
    if (position != reportedPosition_ || buffered_ != reportedBuffered_) {
        reportedPosition_ = position;
        reportedBuffered_ = buffered_;
        bridge_.publish("audioplayer.onPlayProgress", {id_, position, buffered_});
    }
    if (segment_ && duration_ > 0 && position >= duration_) {
        libvlc_media_player_stop(player_);
        end(0);
    }
}

void AudioService::acknowledgeState()
{
    if (!statePending_ || !loaded_ || !player_ || ended_ || id_.isEmpty())
        return;
    const auto state = libvlc_media_player_get_state(player_);
    if (state != (wantPlay_ ? libvlc_Playing : libvlc_Paused))
        return;
    statePending_ = false;
    bridge_.publish("audioplayer.onPlayState", {id_, command_, wantPlay_ ? 1 : 2});
}

void AudioService::setVolume(double volume)
{
    volume_ = std::isfinite(volume) ? std::clamp(volume, 0., 1.) : 0.;
    applyVolume();
}

void AudioService::fadeTo(double factor)
{
    fade_.stop();
    if (!fadeEnabled_ || fadeFactor_ == factor) {
        fadeFactor_ = factor;
        applyVolume();
        return;
    }
    fade_.setStartValue(fadeFactor_);
    fade_.setEndValue(factor);
    fade_.start();
}

void AudioService::applyVolume()
{
    if (player_)
        libvlc_audio_set_volume(player_, int(std::lround(volume_ * fadeFactor_ * 100)));
}

}
