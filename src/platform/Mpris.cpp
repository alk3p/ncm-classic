#include "Mpris.h"

#include "bridge/Bridge.h"
#include "web/WebHost.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QUrl>

namespace ncm {

namespace {

constexpr auto objectPath = "/org/mpris/MediaPlayer2";
constexpr auto serviceName = "org.mpris.MediaPlayer2." NCM_APP_ID;
constexpr auto playerInterface = "org.mpris.MediaPlayer2.Player";
constexpr double microseconds = 1'000'000;

}

class MprisRoot final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ yes CONSTANT)
    Q_PROPERTY(bool CanRaise READ yes CONSTANT)
    Q_PROPERTY(bool HasTrackList READ no CONSTANT)
    Q_PROPERTY(QString Identity READ identity CONSTANT)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry CONSTANT)
    Q_PROPERTY(QStringList SupportedUriSchemes READ uriSchemes CONSTANT)
    Q_PROPERTY(QStringList SupportedMimeTypes READ mimeTypes CONSTANT)

public:
    MprisRoot(QObject *object, Bridge &bridge)
        : QDBusAbstractAdaptor(object)
        , bridge_(bridge)
    {}

    bool yes() const { return true; }
    bool no() const { return false; }
    QString identity() const { return NCM_DISPLAY_NAME; }
    QString desktopEntry() const { return NCM_APP_ID; }
    QStringList uriSchemes() const { return {"file", "orpheus"}; }
    QStringList mimeTypes() const
    {
        return {"audio/mpeg", "audio/flac", "audio/ogg", "audio/mp4", "audio/wav"};
    }

public slots:
    void Raise() { bridge_.publish("player.onrequestchangetomain"); }
    void Quit() { bridge_.publishTo("main", "winhelper.onclose"); }

private:
    Bridge &bridge_;
};

class MprisPlayer final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ rate CONSTANT)
    Q_PROPERTY(double MaximumRate READ rate CONSTANT)
    Q_PROPERTY(bool CanGoNext READ yes CONSTANT)
    Q_PROPERTY(bool CanGoPrevious READ yes CONSTANT)
    Q_PROPERTY(bool CanPlay READ yes CONSTANT)
    Q_PROPERTY(bool CanPause READ yes CONSTANT)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ yes CONSTANT)

public:
    MprisPlayer(QObject *object, Bridge &bridge, WebHost &web);

    QString playbackStatus() const { return status_; }
    QString loopStatus() const { return loop_; }
    void setLoopStatus(const QString &);
    double rate() const { return 1; }
    void setRate(double) {}
    bool shuffle() const { return shuffle_; }
    void setShuffle(bool);
    QVariantMap metadata() const { return metadata_; }
    double volume() const { return volume_; }
    void setVolume(double);
    qlonglong position() const { return position_; }
    bool yes() const { return true; }
    bool canSeek() const { return !playId_.isEmpty(); }

public slots:
    void Next() { action("next"); }
    void Previous() { action("prev"); }
    void Pause();
    void PlayPause() { action("play"); }
    void Stop() { action("stop"); }
    void Play();
    void Seek(qlonglong offset);
    void SetPosition(const QDBusObjectPath &track, qlonglong position);
    void OpenUri(const QString &uri);

signals:
    void Seeked(qlonglong position);

private:
    void called(const QString &method, const QJsonArray &args);
    void observed(const QString &name, const QJsonArray &args);
    void action(const QString &name) { bridge_.publish("player.onaction", {name}); }
    void menuClick(const char *id) { bridge_.publish("winhelper.onmenuclick", {id, false}); }
    void setStatus(const QString &status);
    void changed(const QVariantMap &properties);

    Bridge &bridge_;
    WebHost &web_;
    QString status_ = "Stopped";
    QString loop_ = "None";
    QString playId_;
    bool shuffle_ = false;
    double volume_ = .5;
    qlonglong position_ = 0;
    QVariantMap metadata_;
};

MprisPlayer::MprisPlayer(QObject *object, Bridge &bridge, WebHost &web)
    : QDBusAbstractAdaptor(object)
    , bridge_(bridge)
    , web_(web)
{
    connect(&bridge_, &Bridge::invoked, this, &MprisPlayer::called);
    connect(
        &bridge_,
        &Bridge::event,
        this,
        [this](const QString &target, const QString &name, const QJsonArray &args) {
            if (target.isEmpty())
                observed(name, args);
        });
}

void MprisPlayer::called(const QString &method, const QJsonArray &args)
{
    if (method == "player.setInfo") {
        const auto info = args.at(0).toObject();
        const auto id = info.value("playId").toVariant().toString().toUtf8();
        const auto track = QString(objectPath) + "/track/"
                           + QString::fromLatin1(
                               QCryptographicHash::hash(id, QCryptographicHash::Sha256).toHex());
        const auto length = metadata_.value("mpris:length");
        metadata_ = {
            {"mpris:trackid", QVariant::fromValue(QDBusObjectPath(track))},
            {"xesam:title", info.value("songName").toString()},
            {"xesam:artist", QStringList{info.value("artistName").toString()}},
            {"xesam:album", info.value("albumName").toString()},
            {"mpris:artUrl", info.value("url").toString()},
        };
        if (length.isValid())
            metadata_["mpris:length"] = length;
        changed({{"Metadata", metadata_}});
    } else if (method == "player.setTotalTime") {
        metadata_["mpris:length"] = qlonglong(numberArg(args, 0) * 1000);
        changed({{"Metadata", metadata_}});
    } else if (method == "player.setPlayMode") {
        const auto mode = stringArg(args, 0);
        shuffle_ = mode == "playrandom";
        loop_ = mode == "playorder" ? "Playlist" : mode == "playcycle" ? "Track" : "None";
        changed({{"LoopStatus", loop_}, {"Shuffle", shuffle_}});
    } else if (method == "player.setMiniPlayerState") {
        const auto state = args.at(0).toObject().value("playstate").toVariant().toInt();
        setStatus(state == 0 ? "Playing" : "Paused");
    } else if (method == "audioplayer.load") {
        playId_ = stringArg(args, 0);
        changed({{"CanSeek", true}});
    } else if (method == "audioplayer.stop") {
        playId_.clear();
        position_ = 0;
        status_ = "Stopped";
        changed({{"PlaybackStatus", status_}, {"CanSeek", false}});
    }
}

void MprisPlayer::observed(const QString &name, const QJsonArray &args)
{
    if (name == "audioplayer.onPlayProgress") {
        position_ = qlonglong(numberArg(args, 1) * microseconds);
    } else if (name == "audioplayer.onVolume") {
        volume_ = numberArg(args, 3);
        changed({{"Volume", volume_}});
    } else if (name == "audioplayer.onSeek" && numberArg(args, 2) == 0) {
        position_ = qlonglong(numberArg(args, 3) * microseconds);
        emit Seeked(position_);
    } else if (name == "audioplayer.onPlayState") {
        setStatus(numberArg(args, 2) == 1 ? "Playing" : "Paused");
    } else if (name == "audioplayer.onEnd") {
        setStatus("Stopped");
    }
}

void MprisPlayer::setStatus(const QString &status)
{
    status_ = status;
    changed({{"PlaybackStatus", status_}});
}

void MprisPlayer::changed(const QVariantMap &properties)
{
    auto signal = QDBusMessage::createSignal(
        objectPath, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    signal << QString(playerInterface) << properties << QStringList();
    QDBusConnection::sessionBus().send(signal);
}

void MprisPlayer::setLoopStatus(const QString &mode)
{
    if (mode == "Track")
        menuClick("1041");
    else if (mode == "Playlist")
        menuClick("1040");
    else if (mode == "None")
        menuClick("1043");
}

void MprisPlayer::setShuffle(bool on)
{
    menuClick(on ? "1042" : "1040");
}

void MprisPlayer::setVolume(double volume)
{
    bridge_.publish("player.onminivolumechange", {std::clamp(volume, 0., 1.)});
}

void MprisPlayer::Pause()
{
    if (status_ == "Playing")
        action("pause");
}

void MprisPlayer::Play()
{
    if (status_ != "Playing")
        action("play");
}

void MprisPlayer::Seek(qlonglong offset)
{
    SetPosition(
        metadata_.value("mpris:trackid").value<QDBusObjectPath>(),
        std::max<qlonglong>(0, position_ + offset));
}

void MprisPlayer::SetPosition(const QDBusObjectPath &track, qlonglong position)
{
    const auto current = metadata_.value("mpris:trackid").value<QDBusObjectPath>();
    if (playId_.isEmpty() || track.path() != current.path())
        return;
    bridge_.call(
        {"mpris", "mpris:seek"},
        "audioplayer.seek",
        {playId_, "mpris-seek", position / microseconds},
        [](const QJsonArray &) {});
}

void MprisPlayer::OpenUri(const QString &uri)
{
    const QUrl url(uri);
    if (url.isLocalFile()) {
        web_.resources().grantFile(url.toLocalFile());
        bridge_.publish("app.onplaylocalmusic", {url.toLocalFile()});
    } else if (url.scheme() == "orpheus") {
        bridge_.publish("app.onwebmessage", {uri});
    }
}

Mpris::Mpris(Bridge &bridge, WebHost &web, QObject *parent)
    : QObject(parent)
{
    new MprisRoot(this, bridge);
    new MprisPlayer(this, bridge, web);
    auto bus = QDBusConnection::sessionBus();
    if (bus.registerService(serviceName))
        bus.registerObject(objectPath, this, QDBusConnection::ExportAdaptors);
}

Mpris::~Mpris()
{
    QDBusConnection::sessionBus().unregisterService(serviceName);
}

}

#include "Mpris.moc"
