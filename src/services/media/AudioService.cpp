#include "AudioService.h"

#include "bridge/Bridge.h"
#include "web/WebHost.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QFile>
#include <QJsonDocument>

namespace ncm {

namespace {

constexpr int pollMs = 100;
constexpr int fadeMs = 500;
constexpr int invalidMedia = 2;

}

AudioService::AudioService(
    Bridge &bridge, NativeNetwork &network, const RuntimePaths &paths, WebHost &web, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , cache_(bridge, network, paths)
{
    poll_.setInterval(pollMs);
    connect(&poll_, &QTimer::timeout, this, [this] { send({"sample"}); });

    endTimer_.setSingleShot(true);
    endTimer_.setTimerType(Qt::PreciseTimer);
    connect(&endTimer_, &QTimer::timeout, this, [this] { end(0); });

    fade_.setDuration(fadeMs);
    connect(&fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &factor) {
        fadeFactor_ = factor.toDouble();
        applyVolume();
    });
    connect(&fade_, &QVariantAnimation::finished, this, [this] {
        if (!wantPlay_)
            send({"pause"});
    });

    registerHandlers();
    startPlayer(web);
}

void AudioService::registerHandlers()
{
    for (const auto name : {"audioplayer.enmeratorDevices", "audioplayer.init"})
        bridge_.addHandler(name, [](const QJsonArray &, Bridge::Reply done) { done({}); });

    const auto current = [this](const QJsonArray &a) {
        return !id_.isEmpty() && stringArg(a, 0) == id_;
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
    bridge_.addHandler("audioplayer.stop", [this, current](const QJsonArray &a, Bridge::Reply done) {
        if (current(a))
            stop();
        done({});
    });
    bridge_.addHandler("audioplayer.seek", [this, current](const QJsonArray &a, Bridge::Reply done) {
        if (current(a)) {
            auto position = std::max(0., numberArg(a, 2));
            if (duration_ > 0)
                position = std::min(position, duration_);
            if (loaded_) {
                endTimer_.stop();
                send({"seek", position + offset_});
            }
            bridge_.publish("audioplayer.onSeek", {id_, a.at(1), loaded_ ? 0 : -1, position});
        }
        done({});
    });
    bridge_.addHandler("audioplayer.setVolume", [this](const QJsonArray &a, Bridge::Reply done) {
        volume_ = std::clamp(numberArg(a, 2), 0., 1.);
        applyVolume();
        bridge_.publish("audioplayer.onVolume", {a.at(0), a.at(1), 0, volume_});
        done({});
    });
    bridge_.addHandler("audioplayer.switchEffect", [this](const QJsonArray &a, Bridge::Reply done) {
        for (const auto value : a.at(1).toArray())
            if (const auto option = value.toObject(); option.value("name") == "fade")
                fadeEnabled_ = option.value("on").toBool();
        if (!fadeEnabled_ && fade_.state() == QAbstractAnimation::Running) {
            fadeTo(wantPlay_ ? 1 : 0);
            if (!wantPlay_)
                send({"pause"});
        }
        done({});
    });
}

void AudioService::startPlayer(WebHost &web)
{
    const auto page = cache_.playerPage();
    QFile script(":/ncm/assets/channel/audio.js");
    if (page.isEmpty() || !script.open(QIODevice::ReadOnly))
        return;
    auto view = web.createHiddenView();
    if (!view)
        return;
    player_.emplace(std::move(*view));

    using enum saucer::webview::event;
    player_->on<navigate>([page](const saucer::navigation &nav) {
        const QUrl url(QString::fromStdString(nav.url().string()));
        return !nav.new_window() && url == page ? saucer::policy::allow : saucer::policy::block;
    });
    player_->on<message>([this](std::string_view message) { return receive(message); });
    player_->inject({.code = script.readAll().toStdString(), .run_at = saucer::script::time::ready});
    player_->set_url(page.toString(QUrl::FullyEncoded).toStdString());
}

saucer::status AudioService::receive(std::string_view raw)
{
    const auto message
        = QJsonDocument::fromJson(QByteArray(raw.data(), qsizetype(raw.size()))).array();
    const auto event = message.at(0).toString();
    if (event.isEmpty())
        return saucer::status::unhandled;
    if (event == "ready") {
        if (!std::exchange(playerReady_, true) && !source_.isEmpty())
            open();
    } else if (source_.isEmpty() || message.at(1).toString() != source_.toString()) {
        return saucer::status::handled;
    } else if (event == "sample") {
        update(message.at(2).toObject());
    } else if (event == "ended") {
        end(0);
    } else if (event == "error") {
        end(invalidMedia);
    }
    return saucer::status::handled;
}

void AudioService::send(const QJsonArray &command)
{
    if (player_ && playerReady_)
        player_->execute(("window.__ncmAudio(" + jsLiteral(command) + ");").toStdString());
}

void AudioService::load(const QString &id, const QJsonObject &info)
{
    stop();
    id_ = id;
    offset_ = info.value("starttime").toDouble();
    duration_ = info.value("duration").toDouble();

    auto path = info.value("path").toString();
    if (path.isEmpty())
        path = info.value("musicurl").toString();
    if (path.isEmpty() || path == "cacheurl")
        path = cache_.cached(info);
    const QUrl url(path);
    if (player_ && !path.isEmpty())
        source_ = url.scheme() == "http" || url.scheme() == "https"
                      ? cache_.stream(path, info)
                      : cache_.file(url.isLocalFile() ? url.toLocalFile() : path);

    if (source_.isEmpty())
        end(invalidMedia);
    else if (playerReady_)
        open();
}

void AudioService::open()
{
    send({"open", source_.toString(), offset_});
    applyVolume();
    setBuffering(true);
    poll_.start();
}

void AudioService::play(const QString &command)
{
    wantPlay_ = true;
    command_ = command;
    statePending_ = true;
    if (!loaded_)
        return;
    fadeTo(1);
    send({"play"});
}

void AudioService::pause(const QString &command)
{
    wantPlay_ = false;
    command_ = command;
    statePending_ = true;
    if (fadeEnabled_ && playing_ && fadeFactor_ > 0) {
        fadeTo(0);
        return;
    }
    fade_.stop();
    fadeFactor_ = 0;
    applyVolume();
    send({"pause"});
}

void AudioService::stop()
{
    poll_.stop();
    endTimer_.stop();
    fade_.stop();
    id_.clear();
    loaded_ = playing_ = wantPlay_ = statePending_ = false;
    fadeFactor_ = 0;
    buffering_ = -1;
    reportedPosition_ = reportedBuffered_ = -1;
    if (const auto source = std::exchange(source_, QUrl()); !source.isEmpty()) {
        send({"close"});
        cache_.release(source);
    }
}

void AudioService::end(int code)
{
    const auto id = id_;
    if (id.isEmpty())
        return;
    stop();
    bridge_.publish(
        code == 0 ? "audioplayer.onEnd" : "audioplayer.onLoad", {id, QJsonObject{{"code", code}}});
}

void AudioService::update(const QJsonObject &sample)
{
    const bool loaded = loaded_ || sample.value("loaded").toBool();
    setBuffering(!loaded || sample.value("waiting").toBool());
    if (!loaded)
        return;
    if (!loaded_) {
        loaded_ = true;
        if (duration_ <= 0)
            duration_ = std::max(0., sample.value("duration").toDouble() - offset_);
        bridge_
            .publish("audioplayer.onLoad", {id_, QJsonObject{{"code", 0}, {"duration", duration_}}});
        if (wantPlay_)
            play(command_);
    }

    const bool paused = sample.value("paused").toBool();
    playing_ = !paused && !buffering_;
    if (statePending_ && (wantPlay_ ? playing_ : paused)) {
        statePending_ = false;
        bridge_.publish("audioplayer.onPlayState", {id_, command_, wantPlay_ ? 1 : 2});
    }

    const auto position = std::max(0., sample.value("position").toDouble() - offset_);
    const auto buffered
        = duration_ > 0
              ? std::clamp((sample.value("buffered").toDouble() - offset_) / duration_, 0., 1.)
              : 0.;
    if (position != reportedPosition_ || buffered != reportedBuffered_) {
        reportedPosition_ = position;
        reportedBuffered_ = buffered;
        bridge_.publish("audioplayer.onPlayProgress", {id_, position, buffered});
    }

    if (playing_ && duration_ > 0)
        endTimer_.start(int(std::ceil((duration_ - std::min(position, duration_)) * 1000)));
    else
        endTimer_.stop();
}

void AudioService::setBuffering(bool buffering)
{
    if (buffering_ == int(buffering))
        return;
    buffering_ = buffering;
    bridge_.publish("audioplayer.onBuffering", {id_, buffering_});
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
    if (!source_.isEmpty())
        send({"volume", volume_ * fadeFactor_});
}

}
