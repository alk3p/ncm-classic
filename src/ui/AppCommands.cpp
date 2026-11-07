#include "AppCommands.h"

#include "Skin.h"
#include "app/LegacyIdentity.h"
#include "bridge/Bridge.h"
#include "bridge/JsonArgs.h"
#include "web/WebHost.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QUrlQuery>

namespace ncm {

namespace {

void quit()
{
    QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(0); });
}

}

AppCommands::AppCommands(
    Bridge &bridge, const RuntimePaths &paths, WebHost &web, Skin &skin, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , paths_(paths)
    , web_(web)
    , skin_(skin)
    , settings_(Profile(paths))
{
    skin_.load(settings_.get("Skins", "current-skin", "default").toString());

    registerSettings();
    registerLifetime();
    registerSkins();
    registerUpdates();
}

QJsonObject AppCommands::proxyConfig() const
{
    const auto stored = QJsonDocument::fromJson(
        settings_.get("Proxy", "default-key").toString().toUtf8());
    return stored.isObject() ? stored.object() : QJsonObject();
}

void AppCommands::registerSettings()
{
    const auto add = [this](const QString &name, Bridge::Handler handler) {
        bridge_.addHandler("app." + name, std::move(handler));
    };

    add("getLocalConfig", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto key = stringArg(a, 1);
        reply({QJsonValue::fromVariant(
            settings_.get(stringArg(a, 0), key.isEmpty() ? "default-key" : key))});
    });
    add("setLocalConfig", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto group = stringArg(a, 0), key = stringArg(a, 1);
        const bool saved
            = settings_.set(group, key.isEmpty() ? "default-key" : key, a.at(2).toVariant());
        QFile::setPermissions(
            paths_.settingsFile, QFile::ReadOwner | QFile::WriteOwner);
        if (group == "Proxy")
            emit proxyChanged(proxyConfig());
        reply({saved});
    });
    for (const auto name : {"setThumbnail", "sendFeedback"})
        add(name, [](const QJsonArray &, Bridge::Reply reply) { reply({}); });
}

void AppCommands::registerLifetime()
{
    const auto add = [this](const QString &name, Bridge::Handler handler) {
        bridge_.addHandler("app." + name, std::move(handler));
    };

    add("getAppStartType", [](const QJsonArray &, Bridge::Reply reply) { reply({"normal"}); });
    add("getAppStartCommand", [](const QJsonArray &, Bridge::Reply reply) {
        reply({QJsonObject{{"webcmd", qApp->property("ncm.webcmd").toString()}}});
    });
    add("getDefaultMusicPlayPath", [](const QJsonArray &, Bridge::Reply reply) {
        reply({qApp->property("ncm.startupAudio").toString()});
    });
    for (const auto name : {"exit", "forceExit"})
        add(name, [](const QJsonArray &, Bridge::Reply reply) {
            reply({});
            quit();
        });
    add("exitWithMode", [](const QJsonArray &a, Bridge::Reply reply) {
        const auto mode = stringArg(a, 0);
        if (mode != "restart" && mode != "quitrun" && mode != "install") {
            reply(Bridge::failure("unsupported_exit_mode", mode));
            return;
        }
        if (mode == "restart")
            qApp->setProperty("ncm.restart", true);
        reply({});
        quit();
    });
    add("exitWithMove", [this](const QJsonArray &a, Bridge::Reply reply) {
        if (stringArg(a, 0) != "moverun") {
            reply(Bridge::failure("unsupported_exit_mode", stringArg(a, 0)));
            return;
        }
        QUrl url("orpheus://orpheus/pub/move.html");
        url.setQuery(QUrlQuery{{"src", stringArg(a, 1)}, {"dest", stringArg(a, 2)}});
        if (auto *main = web_.window("main")) {
            main->hide();
            main->load(QUrl("about:blank"));
        }
        web_.createWindow("move", url);
        reply({true});
    });
}

void AppCommands::registerSkins()
{
    const auto add = [this](const QString &name, Bridge::Handler handler) {
        bridge_.addHandler("app." + name, std::move(handler));
    };

    add("loadSkinPackets", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto name = stringArg(a, 1);
        const auto hsl = jsonObject(a.at(2)).value("btn_color").toObject();
        const auto buttonColor = hsl.isEmpty() ? QColor()
                                               : QColor::fromHslF(
                                                     float(hsl.value("h").toDouble() / 360),
                                                     float(hsl.value("s").toDouble() / 100),
                                                     float(hsl.value("l").toDouble() / 100));
        const bool loaded = stringArg(a, 0) == "default" && skin_.load(name, buttonColor);
        if (loaded)
            settings_.set("Skins", "current-skin", name);
        reply({loaded});
    });
    add("querySkinPackets", [this](const QJsonArray &, Bridge::Reply reply) {
        const QDir skins(":/ncm/assets/webapp/style/skin");
        QJsonArray packets;
        for (const auto &name : skins.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile info(skins.filePath(name + "/web/info"));
            if (!info.open(QIODevice::ReadOnly))
                continue;
            const auto metadata = QJsonDocument::fromJson(info.readAll()).object();
            if (metadata["version"].toInt() == 1)
                packets.append(QJsonObject{{"name", name}, {"info", metadata}});
        }
        const QJsonObject found{{"default", packets}};
        bridge_.publish("app.onqueryskinpackets", {found});
        reply({found});
    });
}

void AppCommands::registerUpdates()
{
    const auto add = [this](const QString &name, Bridge::Handler handler) {
        bridge_.addHandler("update." + name, std::move(handler));
    };

    add("getVersion", [](const QJsonArray &, Bridge::Reply reply) { reply({legacy::version}); });
    add("getVisualVersion", [](const QJsonArray &, Bridge::Reply reply) {
        reply({QJsonObject{{"version", legacy::version}, {"build", "0428"}}});
    });
}

}
