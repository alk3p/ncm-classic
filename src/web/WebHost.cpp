#include "WebHost.h"

#include "NativeView.h"
#include "app/LegacyIdentity.h"
#include "bridge/Bridge.h"

#include <filesystem>
#include <memory>
#include <utility>

#include <QDebug>
#include <QFile>
#include <QGuiApplication>

namespace ncm {

namespace {

QString errorText(const saucer::error &error)
{
    return QString::fromStdString(error.message());
}

}

WebHost::WebHost(Bridge &bridge, const RuntimePaths &paths)
    : bridge_(bridge)
    , paths_(paths)
    , resources_(paths)
{
    connect(
        &bridge_,
        &Bridge::event,
        this,
        [this](const QString &target, const QString &name, const QJsonArray &args) {
            for (const auto &[_, window] : windows_)
                window->deliverEvent(target, name, args);
        });
}

WebHost::~WebHost()
{
    shutdown();
}

std::expected<void, QString> WebHost::start()
{
    QFile bridge(":/ncm/assets/channel/bridge.js");
    QFile network(":/ncm/assets/channel/network.js");
    if (!bridge.open(QIODevice::ReadOnly) || !network.open(QIODevice::ReadOnly))
        return std::unexpected(QStringLiteral("The page bridge scripts are missing."));
    bridgeScript_ = QString::fromUtf8(bridge.readAll() + '\n' + network.readAll());

    auto app = saucer::application::create({.id = NCM_APP_ID, .quit_on_last_window_closed = false});
    if (!app)
        return std::unexpected(errorText(app.error()));
    app_.emplace(std::move(*app));
    if (!native::attachToQtLoop(*app_))
        return std::unexpected(QStringLiteral("The webview cannot join the Qt event loop."));
    saucer::webview::register_scheme("orpheus");
    return {};
}

void WebHost::shutdown()
{
    windows_.clear();
}

WebWindow *WebHost::createWindow(const QString &name, const QUrl &url, bool visible)
{
    if (const auto it = windows_.find(name); it != windows_.end()) {
        it->second->load(url);
        if (visible)
            it->second->show();
        return it->second.get();
    }
    auto window = makeWindow(name, url);
    if (!window)
        return nullptr;
    auto *created = window.get();
    windows_.emplace(name, std::move(window));
    if (visible)
        created->show();
    return created;
}

WebWindow *WebHost::window(const QString &name) const
{
    auto key = name.isEmpty() ? bridge_.callingWindow() : name;
    if (key.isEmpty())
        key = "main";
    const auto it = windows_.find(key);
    return it == windows_.end() ? nullptr : it->second.get();
}

QList<WebWindow *> WebHost::windows() const
{
    QList<WebWindow *> result;
    for (const auto &[_, window] : windows_)
        result.append(window.get());
    return result;
}

void WebHost::destroyWindow(const QString &name)
{
    if (name == "main") {
        if (auto *main = window("main"))
            main->hide();
        return;
    }
    auto node = windows_.extract(name);
    if (node.empty())
        return;
    auto *window = node.mapped().release();
    window->hide();
    window->deleteLater();
}

void WebHost::setCookieSource(std::function<QList<QNetworkCookie>()> source)
{
    cookieSource_ = std::move(source);
}

void WebHost::mirrorCookie(const QNetworkCookie &cookie, bool removed)
{
    if (!cookieStoreAttached_ || windows_.empty())
        return;
    auto &view = windows_.begin()->second->view();
    if (removed)
        native::removeCookie(view, cookie);
    else
        native::setCookie(view, cookie);
}

std::unique_ptr<WebWindow> WebHost::makeWindow(const QString &name, const QUrl &url)
{
    if (!app_)
        return nullptr;
    auto window = saucer::window::create(std::addressof(*app_));
    if (!window) {
        qWarning().noquote() << "Cannot create window" << name << errorText(window.error());
        return nullptr;
    }
    (*window)->set_decorations(saucer::window::decoration::partial);
    (*window)->set_size({1050, 720});

    auto view = saucer::webview::create({
        .window = *window,
        .attributes = false,
        .persistent_cookies = true,
        .storage_path = std::filesystem::path(paths_.webViewData.toStdWString()),
        .user_agent = std::string(legacy::browserUserAgent),
        .browser_flags = {"--autoplay-policy=no-user-gesture-required"},
    });
    if (!view) {
        qWarning().noquote() << "Cannot create webview" << name << errorText(view.error());
        return nullptr;
    }
    view->set_context_menu(false);
    if (qEnvironmentVariableIsSet("NCM_WEB_DEVTOOLS"))
        view->set_dev_tools(true);
    const auto bridge = "window.__ncmWindowId = " + jsLiteral(name) + ";\n" + bridgeScript_;
    view->inject({.code = bridge.toStdString(), .run_at = saucer::script::time::creation});

    auto result = std::make_unique<WebWindow>(*this, name, std::move(*window), std::move(*view));
    result->setTitle(QGuiApplication::applicationDisplayName());
    result->setIcon(":/ncm/assets/misc/netease-cloud-music.svg");
    attachCookieStore(*result);
    result->load(url);
    return result;
}

void WebHost::attachCookieStore(WebWindow &window)
{
    if (std::exchange(cookieStoreAttached_, true) || !cookieSource_)
        return;
    for (const auto &cookie : cookieSource_())
        native::setCookie(window.view(), cookie);
}

}
