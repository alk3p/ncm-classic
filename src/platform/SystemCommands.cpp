#include "SystemCommands.h"

#include "app/LegacyIdentity.h"
#include "bridge/Bridge.h"
#include "bridge/JsonArgs.h"
#include "services/persistence/SettingsStore.h"

#include <QApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFontDatabase>
#include <QNetworkInformation>
#include <QScreen>
#include <QStorageInfo>
#include <QUrl>

namespace ncm {

namespace {

bool online()
{
    const auto *info = QNetworkInformation::instance();
    return !info || info->reachability() == QNetworkInformation::Reachability::Online
           || info->reachability() == QNetworkInformation::Reachability::Unknown;
}

}

SystemCommands::SystemCommands(Bridge &bridge, const RuntimePaths &paths, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
{
    QNetworkInformation::loadDefaultBackend();
    const auto add = [&bridge](const QString &name, Bridge::Handler handler) {
        bridge.addHandler("os." + name, std::move(handler));
    };

    add("queryOsVer", [](const QJsonArray &, Bridge::Reply reply) { reply({legacy::osVersion}); });
    add("getSystemInfo", [](const QJsonArray &a, Bridge::Reply reply) {
        if (stringArg(a, 0) != "monitor") {
            reply({QJsonObject()});
            return;
        }
        const auto *screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        reply({QJsonObject{{"workArea", jsonRect(screen->geometry())}}});
    });
    const auto deviceId = SettingsStore(Profile(paths)).deviceId();
    for (const auto name : {"getDeviceId", "getADDeviceID"})
        add(name, [deviceId](const QJsonArray &, Bridge::Reply reply) { reply({deviceId}); });
    add("getKeyboardState", [](const QJsonArray &, Bridge::Reply reply) { reply({}); });
    add("isOnLine", [&bridge](const QJsonArray &, Bridge::Reply reply) {
        const bool now = online();
        bridge.publish("os.onisonline", {now});
        reply({now});
    });
    bridge.addHandler("app.setWatchNetStatus", [this](const QJsonArray &a, Bridge::Reply reply) {
        watchNetwork(boolArg(a, 0));
        reply({});
    });

    add("checkNativeSupportFonts", [](const QJsonArray &a, Bridge::Reply reply) {
        const auto families = QFontDatabase::families();
        QJsonArray found;
        for (const auto family : a.at(0).toArray())
            if (families.contains(family.toString(), Qt::CaseInsensitive))
                found.append(family);
        if (found.isEmpty())
            found.append(QApplication::font().family());
        reply({"success", found});
    });
    add("querySystemFonts", [](const QJsonArray &, Bridge::Reply reply) {
        reply({"success", QJsonArray::fromStringList(QFontDatabase::families())});
    });

    add("isFileExist", [](const QJsonArray &a, Bridge::Reply reply) {
        reply({QFileInfo::exists(stringArg(a, 0))});
    });
    add("getDiskSpace", [](const QJsonArray &a, Bridge::Reply reply) {
        const QStorageInfo disk(stringArg(a, 0));
        const QJsonObject
            space{{"total", double(disk.bytesTotal())}, {"free", double(disk.bytesAvailable())}};
        reply({QString::fromUtf8(QJsonDocument(space).toJson(QJsonDocument::Compact))});
    });
    add("navigateExternal", [](const QJsonArray &a, Bridge::Reply reply) {
        const QUrl url(stringArg(a, 0));
        const bool external = url.scheme() == "http" || url.scheme() == "https"
                              || url.scheme() == "mailto";
        reply({external && QDesktopServices::openUrl(url)});
    });
    add("shellExplor", [](const QJsonArray &a, Bridge::Reply reply) {
        const QFileInfo target(stringArg(a, 0));
        const auto folder = target.isFile() ? target.absolutePath() : target.filePath();
        reply({QDesktopServices::openUrl(QUrl::fromLocalFile(folder))});
    });
    add("shellOpen", [](const QJsonArray &a, Bridge::Reply reply) {
        reply({QDesktopServices::openUrl(QUrl::fromLocalFile(stringArg(a, 0)))});
    });
}

void SystemCommands::watchNetwork(bool watch)
{
    disconnect(watch_);
    const auto *info = QNetworkInformation::instance();
    if (!watch || !info)
        return;
    watch_ = connect(
        info,
        &QNetworkInformation::reachabilityChanged,
        this,
        [this](QNetworkInformation::Reachability reachability) {
            using enum QNetworkInformation::Reachability;
            if (reachability == Online || reachability == Disconnected)
                bridge_.publish("app.onnetstatus", {reachability == Online});
        });
}

}
