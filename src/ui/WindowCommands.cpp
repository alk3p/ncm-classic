#include "WindowCommands.h"

#include "DialogCommands.h"
#include "PlayerWindows.h"
#include "bridge/Bridge.h"
#include "bridge/JsonArgs.h"
#include "web/WebHost.h"

#include <utility>

#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QMimeData>
#include <QWidget>

namespace ncm {

WindowCommands::WindowCommands(
    Bridge &bridge, WebHost &web, PlayerWindows &players, DialogCommands &dialogs, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , web_(web)
    , players_(players)
    , dialogs_(dialogs)
{
    registerWebWindows();
    registerGeometry();
    registerClipboard();
}

void WindowCommands::registerWebWindows()
{
    const auto add = [this](const QString &name, Bridge::Handler handler) {
        bridge_.addHandler("winhelper." + name, std::move(handler));
    };

    add("initMainWindow", [this](const QJsonArray &, Bridge::Reply reply) {
        if (auto *main = web_.window("main"))
            main->show();
        reply({});
    });
    add("loadingFinished", [](const QJsonArray &, Bridge::Reply reply) { reply({}); });
    add("launchWindow", [this](const QJsonArray &a, Bridge::Reply reply) {
        const QUrl url(stringArg(a, 0));
        if (!ResourceHandler::isAppOrigin(url)) {
            reply(Bridge::failure("untrusted_window", url.toString()));
            return;
        }
        auto name = QFileInfo(url.path()).baseName();
        if (name == "login") {
            dialogs_.openSignIn();
            reply({true});
            return;
        }
        if (name == "sns") {
            reply({false});
            return;
        }
        if (name == "app")
            name = "main";
        const auto options = jsonObject(a.at(2));
        auto *window = web_.createWindow(name, url, options.value("visible").toBool(true));
        if (window)
            window->setResizable(options.value("resizable").toBool(true));
        reply({window != nullptr});
    });
    add("destroyWindow", [this](const QJsonArray &, Bridge::Reply reply) {
        web_.destroyWindow(bridge_.callingWindow());
        reply({});
    });
    add("setNativeWindowShow", [this](const QJsonArray &a, Bridge::Reply reply) {
        if (a.isEmpty()) {
            if (auto *window = web_.window())
                window->show();
            reply({});
            return;
        }
        const auto name = stringArg(a, 0);
        const bool visible = boolArg(a, 1);
        if (players_.window(name))
            players_.setVisible(name, visible);
        else if (auto *window = web_.window(name))
            window->setVisible(visible);
        reply({});
    });
    add("showWindow", [this](const QJsonArray &a, Bridge::Reply reply) {
        if (auto *window = web_.window()) {
            const auto mode = stringArg(a, 0);
            if (mode == "hide")
                window->hide();
            else if (mode == "maximize")
                window->showMaximized();
            else if (mode == "minimize")
                window->showMinimized();
            else if (mode == "restore" || mode == "normal")
                window->showNormal();
            else
                window->show();
        }
        reply({});
    });
    add("bringWindowToTop", [this](const QJsonArray &, Bridge::Reply reply) {
        if (auto *window = web_.window())
            window->activate();
        reply({});
    });
    add("setWindowTitle", [this](const QJsonArray &a, Bridge::Reply reply) {
        if (auto *window = web_.window())
            window->setTitle(stringArg(a, 0));
        reply({});
    });
    add("setWindowIconFromLocalFile", [](const QJsonArray &, Bridge::Reply reply) { reply({}); });
}

void WindowCommands::registerGeometry()
{
    const auto add = [this](const QString &name, Bridge::Handler handler) {
        bridge_.addHandler("winhelper." + name, std::move(handler));
    };

    add("setWindowPosition", [this](const QJsonArray &a, Bridge::Reply reply) {
        auto *window = web_.window();
        if (!window) {
            reply({false});
            return;
        }
        const auto o = jsonObject(a.at(0));
        const auto current = window->geometry();
        window->resize(
            {intField(o, "width", current.width()), intField(o, "height", current.height())});
        window->move({intField(o, "x", current.x()), intField(o, "y", current.y())});
        reply({true});
    });
    add("setWindowSizeLimit", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto min = jsonObject(a.at(0)), max = jsonObject(a.at(1));
        if (auto *window = web_.window())
            window->setSizeLimits(
                {intField(min, "x", 0), intField(min, "y", 0)},
                {intField(max, "x", QWIDGETSIZE_MAX), intField(max, "y", QWIDGETSIZE_MAX)});
        reply({});
    });
    add("dragWindow", [this](const QJsonArray &, Bridge::Reply reply) {
        auto *window = web_.window();
        if (window)
            window->startMove();
        reply({window != nullptr});
    });
    add("sizeWindow", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto side = stringArg(a, 0).toLower();
        Qt::Edges edges;
        for (const auto &[word, edge] :
             {std::pair{"left", Qt::LeftEdge},
              {"right", Qt::RightEdge},
              {"top", Qt::TopEdge},
              {"bottom", Qt::BottomEdge}})
            if (side.contains(QLatin1StringView(word)))
                edges |= edge;
        auto *window = web_.window();
        if (window && edges)
            window->startResize(edges);
        reply({window && edges});
    });
}

void WindowCommands::registerClipboard()
{
    const auto add = [this](const QString &name, Bridge::Handler handler) {
        bridge_.addHandler("winhelper." + name, std::move(handler));
    };

    add("getClipBoardData", [](const QJsonArray &, Bridge::Reply reply) {
        reply({QApplication::clipboard()->text()});
    });
    add("setClipBoardData", [](const QJsonArray &a, Bridge::Reply reply) {
        QApplication::clipboard()->setText(stringArg(a, 0));
        reply({});
    });
    add("setFilesToClipboard", [](const QJsonArray &a, Bridge::Reply reply) {
        QList<QUrl> urls;
        for (const auto &file : a.at(1).toArray())
            urls << QUrl::fromLocalFile(file.toString());
        auto *mime = new QMimeData;
        mime->setUrls(urls);
        QByteArray operation = stringArg(a, 0) == "cut" ? "cut" : "copy";
        for (const auto &url : urls)
            operation += '\n' + url.toEncoded();
        mime->setData("x-special/gnome-copied-files", operation);
        QApplication::clipboard()->setMimeData(mime);
        reply({});
    });
}

}
