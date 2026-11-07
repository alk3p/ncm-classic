#include "PlayerWindows.h"

#include "LyricsPainter.h"
#include "MiniPlaylist.h"
#include "Skin.h"
#include "bridge/Bridge.h"
#include "bridge/JsonArgs.h"
#include "services/persistence/Profile.h"
#include "web/ResourceHandler.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QNetworkReply>
#include <QPainter>
#include <QScreen>

namespace ncm {

namespace {

constexpr int tickMs = 33;
constexpr auto miniGroup = "UIMiniPlayer";
constexpr auto lyricsGroup = "UIDesktopLyrics";

QColor hexColor(const QString &hex)
{
    return QColor::fromString(u'#' + hex);
}

Qt::Alignment alignment(const QString &name)
{
    if (name == "left")
        return Qt::AlignLeft;
    if (name == "right")
        return Qt::AlignRight;
    return Qt::AlignHCenter;
}

QJsonArray jsonArray(const QJsonValue &value)
{
    return QJsonDocument::fromJson(value.toString().toUtf8()).array();
}

MiniPlayer::TrackType trackType(const QString &songType)
{
    if (songType == "fm")
        return MiniPlayer::TrackType::Fm;
    if (songType == "djradio")
        return MiniPlayer::TrackType::Radio;
    return MiniPlayer::TrackType::Normal;
}

}

PlayerWindows::PlayerWindows(
    Bridge &bridge,
    const RuntimePaths &paths,
    ResourceHandler &resources,
    Skin &skin,
    QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , paths_(paths)
    , resources_(resources)
    , settings_(Profile(paths))
    , mini_(skin)
    , lyrics_(skin)
    , volume_(skin)
    , slogan_(tr("网易云音乐  听见好时光"))
{
    restorePosition(&mini_, miniGroup);
    restorePosition(&lyrics_, lyricsGroup);
    ticker_.setInterval(tickMs);
    connect(&ticker_, &QTimer::timeout, this, &PlayerWindows::tick);

    connect(
        &bridge_,
        &Bridge::event,
        this,
        [this](const QString &target, const QString &name, const QJsonArray &args) {
            if (!target.isEmpty())
                return;
            if (name == "audioplayer.onPlayProgress")
                clock_.report(qint64(numberArg(args, 1) * 1000));
            else if (name == "audioplayer.onSeek" && numberArg(args, 2) == 0)
                clock_.jump(qint64(numberArg(args, 3) * 1000));
            else if (name == "audioplayer.onLoad" && intField(jsonObject(args.at(1)), "code") == 0)
                clock_.jump(0);
            else
                return;
            tick();
        });

    registerQueue();
    registerMiniPlayer();
    registerLyrics();
    connectWindows();
}

PlayerWindows::~PlayerWindows()
{
    savePosition(&mini_, miniGroup);
    savePosition(&lyrics_, lyricsGroup);
}

QWidget *PlayerWindows::window(const QString &name)
{
    if (name == "mini_player")
        return &mini_;
    if (name == "desktop_lyrics")
        return &lyrics_;
    return nullptr;
}

void PlayerWindows::setVisible(const QString &name, bool visible)
{
    auto *window = this->window(name);
    if (!window)
        return;
    if (!visible) {
        savePosition(window, name == "mini_player" ? miniGroup : lyricsGroup);
        window->hide();
    } else {
        window->show();
    }
    ticker_.start();
}

void PlayerWindows::publish(const QString &name, const QJsonArray &args)
{
    bridge_.publish("player." + name, args);
}

void PlayerWindows::connectWindows()
{
    connect(&mini_, &MiniPlayer::action, this, [this](const QString &name) {
        publish("onaction", {name});
    });
    connect(&mini_, &MiniPlayer::fullModeRequested, this, [this](bool detail) {
        publish("onrequestchangetomain", {detail ? 1 : 0});
    });
    connect(&mini_, &MiniPlayer::closeRequested, this, [this] { publish("onrequestclose"); });
    connect(&mini_, &MiniPlayer::contextMenuRequested, this, [this] { publish("oncontextmenu"); });
    connect(&mini_, &MiniPlayer::likeClicked, this, [this] { publish("onlikeclick"); });
    connect(&mini_, &MiniPlayer::favourClicked, this, [this](int mark) {
        publish("onfavour", {mark});
    });
    connect(&mini_, &MiniPlayer::trashClicked, this, [this] { publish("onrecyle"); });

    auto &playlist = mini_.playlist();
    connect(&playlist, &MiniPlaylist::playRequested, this, [this](const QString &id) {
        publish("onrequestplay", {id});
    });
    connect(&playlist, &MiniPlaylist::menuRequested, this, [this](const QString &id) {
        publish("onmenu", {id});
    });
    connect(&playlist, &MiniPlaylist::shortcut, this, [this](int id, const QStringList &tracks) {
        publish(
            "onshortcut", {QJsonObject{{"id", id}, {"tracks", QJsonArray::fromStringList(tracks)}}});
    });

    connect(&volume_, &VolumeWindow::volumeChanged, this, [this](double volume) {
        publish("onminivolumechange", {volume});
    });

    connect(&lyrics_, &DesktopLyrics::action, this, [this](const QString &name) {
        publish("ondesktoplyricaction", {name});
    });
    connect(&lyrics_, &DesktopLyrics::wheeled, this, [this](int delta) {
        publish("ondesktopmousewheel", {0, delta, 0, delta});
    });
}

void PlayerWindows::registerQueue()
{
    const auto add = [this](const QString &name, Bridge::Handler handler) {
        bridge_.addHandler("player." + name, std::move(handler));
    };
    auto &playlist = mini_.playlist();

    add("setInfo", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto info = jsonObject(a.at(0));
        mini_.setTrack(
            info["songName"].toString(),
            info["artistName"].toString(),
            trackType(info["songType"].toString()));
        reply({});
    });
    add("setCurrentPlay", [&playlist](const QJsonArray &a, Bridge::Reply reply) {
        playlist.setCurrent(stringArg(a, 0));
        reply({});
    });
    add("removeAll", [&playlist](const QJsonArray &, Bridge::Reply reply) {
        playlist.clear();
        reply({});
    });
    add("addListElement", [&playlist](const QJsonArray &a, Bridge::Reply reply) {
        playlist.append(jsonArray(a.at(0)));
        reply({});
    });
    add("deleteListElement", [&playlist](const QJsonArray &a, Bridge::Reply reply) {
        QStringList ids;
        for (const auto &id : jsonArray(a.at(0)))
            ids << id.toVariant().toString();
        playlist.remove(ids);
        reply({});
    });
    add("setMiniPlayerState", [this](const QJsonArray &a, Bridge::Reply reply) {
        setPlaying(intField(jsonObject(a.at(0)), "playstate", 2) == 0);
        reply({});
    });
    for (const auto name : {"setPlayMode", "setTotalTime"})
        add(name, [](const QJsonArray &, Bridge::Reply reply) { reply({}); });
}

void PlayerWindows::registerMiniPlayer()
{
    const auto add = [this](const QString &name, Bridge::Handler handler) {
        bridge_.addHandler("player." + name, std::move(handler));
    };

    add("setCover", [this](const QJsonArray &a, Bridge::Reply reply) {
        loadCover(QUrl(stringArg(a, 0)));
        reply({});
    });
    add("setCoverDefault", [this](const QJsonArray &, Bridge::Reply reply) {
        if (coverRequest_)
            coverRequest_->abort();
        mini_.setCover({});
        reply({});
    });
    add("setLikeMark", [this](const QJsonArray &a, Bridge::Reply reply) {
        mini_.setLikeMark(int(numberArg(a, 0, 2)));
        reply({});
    });
    add("setFavour", [this](const QJsonArray &a, Bridge::Reply reply) {
        mini_.setFavour(boolArg(a, 0));
        reply({});
    });
    add("setAlwaysOnTop", [this](const QJsonArray &a, Bridge::Reply reply) {
        mini_.setTopMost(boolArg(a, 0));
        reply({});
    });
    add("updateTooltips", [this](const QJsonArray &a, Bridge::Reply reply) {
        mini_.setFullModeTip(stringArg(a, 0));
        reply({});
    });
    add("showVolume", [this](const QJsonArray &a, Bridge::Reply reply) {
        volume_.setVolume(numberArg(a, 0, volume_.volume()));
        volume_.popup(mini_.volumeAnchor());
        reply({});
    });
    add("mute", [](const QJsonArray &, Bridge::Reply reply) { reply({}); });
}

void PlayerWindows::registerLyrics()
{
    const auto add = [this](const QString &name, Bridge::Handler handler) {
        bridge_.addHandler("player." + name, std::move(handler));
    };

    add("setLyrics", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto source = jsonObject(a.at(0));
        const auto lyrics = Lyrics::parse(source["krc"].toString(), source["lrc"].toString());
        if (lyrics.isEmpty()) {
            showLyricsMessage(tr("暂无歌词"));
        } else {
            lyrics_.setLyrics(lyrics);
            mini_.setLyrics(lyrics);
        }
        reply({});
    });
    add("setOffset", [this](const QJsonArray &a, Bridge::Reply reply) {
        offset_ = qint64(numberArg(a, 0));
        tick();
        reply({});
    });
    add("setLRCEmpty", [this](const QJsonArray &, Bridge::Reply reply) {
        showLyricsMessage(tr("暂无歌词"));
        reply({});
    });
    add("setMusicOnly", [this](const QJsonArray &, Bridge::Reply reply) {
        showLyricsMessage(tr("纯音乐，请欣赏"));
        reply({});
    });
    add("setLRCDJ", [this](const QJsonArray &, Bridge::Reply reply) {
        showLyricsMessage(slogan_);
        reply({});
    });
    add("setLRCSlogan", [this](const QJsonArray &a, Bridge::Reply reply) {
        slogan_ = stringArg(a, 0);
        showLyricsMessage(slogan_);
        reply({});
    });

    add("setLRCFont", [this](const QJsonArray &a, Bridge::Reply reply) {
        lyrics_.setLyricsFont(int(numberArg(a, 0, 36)), stringArg(a, 2));
        reply({});
    });
    add("setLRCColor", [this](const QJsonArray &a, Bridge::Reply reply) {
        lyrics_.setColors(
            {hexColor(stringArg(a, 0)), hexColor(stringArg(a, 1))},
            {hexColor(stringArg(a, 2)), hexColor(stringArg(a, 3))});
        reply({});
    });
    add("setLineMode", [this](const QJsonArray &a, Bridge::Reply reply) {
        lyrics_.setSingleLine(boolArg(a, 0));
        reply({});
    });
    add("setTextAlign", [this](const QJsonArray &a, Bridge::Reply reply) {
        lyrics_.setAlignment(alignment(stringArg(a, 0)), alignment(stringArg(a, 1)));
        reply({});
    });
    add("setDesktopLyricTopMost", [this](const QJsonArray &a, Bridge::Reply reply) {
        lyrics_.setTopMost(boolArg(a, 0));
        reply({});
    });
    add("setLock", [this](const QJsonArray &a, Bridge::Reply reply) {
        lyrics_.setLocked(boolArg(a, 0));
        reply({});
    });
    add("updateDesktopLrcTooltips", [this](const QJsonArray &a, Bridge::Reply reply) {
        lyrics_.setButtonTips(stringArg(a, 0), stringArg(a, 1), stringArg(a, 2));
        reply({});
    });
    add("renderLRCImage", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto text = stringArg(a, 0);
        const auto &look = lyrics_.look();
        QImage image(
            QFontMetrics(look.font).horizontalAdvance(text) + 16,
            lyricsLineHeight(look),
            QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paintLyricsLine(painter, image.rect(), Qt::AlignHCenter, text, 0.5, look);
        painter.end();
        const auto file = Profile(paths_).relativeFile(stringArg(a, 1));
        const bool saved = !file.isEmpty() && image.save(file);
        publish("onRenderLRCImageResult", {saved, image.width(), image.height()});
        reply({saved});
    });
    for (const auto name : {"simulateKRC", "setFont", "setOutlineColor"})
        add(name, [](const QJsonArray &, Bridge::Reply reply) { reply({}); });
}

void PlayerWindows::setPlaying(bool playing)
{
    clock_.setPlaying(playing);
    mini_.setPlaying(playing);
    lyrics_.setPlaying(playing);
    if (playing)
        ticker_.start();
}

void PlayerWindows::showLyricsMessage(const QString &message)
{
    lyrics_.setMessage(message);
    mini_.setLyrics({});
}

void PlayerWindows::tick()
{
    if (!clock_.playing() || (!mini_.isVisible() && !lyrics_.isVisible()))
        ticker_.stop();
    const auto time = clock_.now() + offset_;
    mini_.setTime(time);
    lyrics_.setTime(time);
}

void PlayerWindows::loadCover(const QUrl &url)
{
    if (coverRequest_)
        coverRequest_->abort();
    if (url.scheme() == "orpheus") {
        QPixmap cover;
        cover.loadFromData(resources_.read(url));
        mini_.setCover(cover);
        return;
    }
    mini_.setCover({});
    auto *request = covers_.get(QNetworkRequest(url));
    coverRequest_ = request;
    connect(request, &QNetworkReply::finished, this, [this, request] {
        request->deleteLater();
        QPixmap cover;
        if (request->error() == QNetworkReply::NoError && cover.loadFromData(request->readAll()))
            mini_.setCover(cover);
    });
}

void PlayerWindows::restorePosition(QWidget *window, const QString &group)
{
    const auto position = settings_.get(group, "window-position").toPoint();
    const auto *screen = QGuiApplication::screenAt(position + QPoint(20, 20));
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const auto area = screen->availableGeometry();
    window->move(
        std::clamp(position.x(), area.left(), std::max(area.left(), area.right() - window->width())),
        std::clamp(position.y(), area.top(), std::max(area.top(), area.bottom() - window->height())));
}

void PlayerWindows::savePosition(QWidget *window, const QString &group)
{
    settings_.set(group, "window-position", window->pos());
}

}
