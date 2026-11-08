#include "app/RuntimePaths.h"
#include "bridge/Bridge.h"
#include "platform/DesktopIntegration.h"
#include "platform/SingleInstance.h"
#include "platform/SystemCommands.h"
#include "services/media/MediaService.h"
#include "services/network/NativeNetwork.h"
#include "services/persistence/PersistenceService.h"
#include "ui/AppCommands.h"
#include "ui/DialogCommands.h"
#include "ui/MenuStyle.h"
#include "ui/PlayerWindows.h"
#include "ui/PopupMenus.h"
#include "ui/Skin.h"
#include "ui/TrayIcon.h"
#include "ui/WindowCommands.h"
#include "web/WebHost.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLibraryInfo>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QTranslator>
#include <QUrl>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif
#ifdef Q_OS_LINUX
#include "platform/Mpris.h"
#endif

namespace {

constexpr int startupFailure = 2;

struct LaunchArguments
{
    QStringList forwarded;
    QString webCommand;
    QString audioFile;
};

LaunchArguments parseArguments(const QStringList &arguments)
{
    LaunchArguments launch{.forwarded = {arguments.value(0)}};
    bool positionalOnly = false;
    for (const auto &argument : arguments.mid(1)) {
        if (!positionalOnly && argument.startsWith('-')) {
            positionalOnly = argument == "--";
            continue;
        }
        const QUrl url(argument);
        if (url.scheme() == "orpheus") {
            if (launch.webCommand.isEmpty())
                launch.webCommand = argument;
            launch.forwarded.append(argument);
            continue;
        }
        const QFileInfo file(url.isLocalFile() ? url.toLocalFile() : argument);
        if (!file.isFile())
            continue;
        if (launch.audioFile.isEmpty())
            launch.audioFile = file.absoluteFilePath();
        launch.forwarded.append(file.absoluteFilePath());
    }
    return launch;
}

ncm::RuntimePaths makePaths()
{
    const auto music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return {
        .dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
        .configRoot = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
        .cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation),
        .downloadRoot = QDir::isAbsolutePath(music) ? QDir(music).filePath("NCM")
                                                    : QDir::home().filePath("Music/NCM"),
    };
}

bool prepareDirectories(const ncm::RuntimePaths &paths)
{
    for (const auto &directory :
         {paths.dataRoot,
          paths.configRoot,
          paths.cacheRoot,
          paths.storageCache,
          paths.tempFiles,
          paths.webData,
          paths.webViewData}) {
        if (!QDir::isAbsolutePath(directory) || !QFileInfo(directory).isNativePath()) {
            qCritical().noquote() << "Invalid application directory" << directory;
            return false;
        }
#ifdef Q_OS_UNIX
        if (!QFileInfo(directory).isDir() && QDir().mkpath(QFileInfo(directory).absolutePath()))
            ::mkdir(QFile::encodeName(directory).constData(), 0700);
#else
        QDir().mkpath(directory);
#endif
        const QFileInfo info(directory);
        if (!info.isDir() || !info.isWritable()) {
            qCritical().noquote() << "Cannot write to application directory" << directory;
            return false;
        }
    }
    return true;
}

void installTranslators(QApplication &app)
{
    static QTranslator qt, ours;
    const auto qtTranslations = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qt.load("qtbase_zh_CN", qtTranslations) || qt.load("qt_zh_CN", qtTranslations))
        app.installTranslator(&qt);
    if (ours.load(":/ncm/assets/i18n/cloud-music_zh_CN.qm"))
        app.installTranslator(&ours);
}

struct RestartGuard
{
    ~RestartGuard()
    {
        if (qApp->property("ncm.restart").toBool())
            QProcess::startDetached(
                QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
    }
};

}

int main(int argc, char **argv)
{
#if defined(SAUCER_WEBKITGTK)
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
#endif
#ifdef Q_OS_LINUX
    qputenv("QT_QPA_PLATFORM", "xcb;wayland");
    qputenv("GDK_BACKEND", "x11,wayland");
#endif

    QApplication app(argc, argv);
    app.setApplicationName(NCM_APP_ID);
    app.setApplicationDisplayName(NCM_DISPLAY_NAME);
    app.setDesktopFileName(NCM_APP_ID);
    app.setQuitOnLastWindowClosed(false);

    const auto launch = parseArguments(app.arguments());
    app.setProperty("ncm.webcmd", launch.webCommand);
    app.setProperty("ncm.startupAudio", launch.audioFile);

    const auto paths = makePaths();
    const QString icon = ":/ncm/assets/misc/netease-cloud-music.svg";
    app.setWindowIcon(QIcon(icon));
    if (!prepareDirectories(paths))
        return startupFailure;

    RestartGuard restartGuard;
    ncm::SingleInstance instance;
    if (!instance.start(paths.dataRoot, launch.forwarded))
        return 0;
    installTranslators(app);

    ncm::Bridge bridge;
    ncm::WebHost web(bridge, paths);
    if (const auto started = web.start(); !started) {
        QMessageBox::critical(nullptr, "无法启动网页组件", started.error());
        return startupFailure;
    }
    if (!launch.audioFile.isEmpty())
        web.resources().grantFile(launch.audioFile);

    ncm::PersistenceService persistence(bridge, paths);
    ncm::MediaService media(bridge, paths, web);
    auto &network = media.network();
    web.resources().setKnownFileHandler(
        [&persistence](const QString &path) { return persistence.containsMusicFile(path); });
    web.setCookieSource([&network] { return network.cookies(); });
    network.setCookieObserver(
        [&web](const QNetworkCookie &cookie, bool removed) { web.mirrorCookie(cookie, removed); });

    ncm::Skin skin;
    ncm::MenuStyle menuStyle(skin);
    ncm::PlayerWindows players(bridge, paths, web.resources(), skin);
    ncm::TrayIcon tray(bridge, icon, web.resources(), menuStyle);
    ncm::PopupMenus menus(bridge, web.resources(), menuStyle, tray);
    ncm::DialogCommands dialogs(bridge, web, network);
    ncm::WindowCommands windows(bridge, web, players, dialogs);
    ncm::AppCommands appCommands(bridge, paths, web, skin);
    ncm::SystemCommands system(bridge, paths);
    ncm::DesktopIntegration desktop(bridge);
#ifdef Q_OS_LINUX
    ncm::Mpris mpris(bridge, web);
#endif
    network.setProxy(appCommands.proxyConfig());
    QObject::connect(
        &appCommands, &ncm::AppCommands::proxyChanged, &network, &ncm::NativeNetwork::setProxy);

    QObject::connect(&instance, &ncm::SingleInstance::activated, &app, [&](const QStringList &args) {
        if (auto *main = web.window("main"))
            main->activate();
        bridge.publish("app.onforegroundpreinstance");
        for (const auto &argument : args.mid(1)) {
            const QUrl url(argument);
            if (url.scheme() == "orpheus") {
                bridge.publish("app.onwebmessage", {argument});
                continue;
            }
            const QFileInfo file(url.isLocalFile() ? url.toLocalFile() : argument);
            if (!file.isFile())
                continue;
            web.resources().grantFile(file.absoluteFilePath());
            bridge.publish("app.onplaylocalmusic", {file.absoluteFilePath()});
        }
    });
    instance.listen();

    if (!web.createWindow("main", QUrl("orpheus://orpheus/pub/start.html"))) {
        QMessageBox::critical(nullptr, "无法启动网页组件", "主窗口创建失败。");
        return startupFailure;
    }
    const int result = app.exec();
    web.shutdown();
    return result;
}
