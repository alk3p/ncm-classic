#include "WebWindow.h"

#include "NativeView.h"
#include "ResourceHandler.h"
#include "WebHost.h"
#include "bridge/Bridge.h"

#include <cstdint>
#include <vector>

#include <QBuffer>
#include <QDesktopServices>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

namespace ncm {

namespace {

bool isWeb(const QUrl &url)
{
    return url.scheme() == "http" || url.scheme() == "https";
}

QUrl toQUrl(const saucer::url &url)
{
    return QUrl(QString::fromStdString(url.string()));
}

}

QString jsLiteral(const QJsonValue &value)
{
    const auto array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

WebWindow::WebWindow(
    WebHost &host, QString name, std::shared_ptr<saucer::window> window, saucer::webview view)
    : host_(host)
    , name_(std::move(name))
    , window_(std::move(window))
    , view_(std::move(view))
    , frame_(window_->decorations())
{
    resizeEnd_.setSingleShot(true);
    resizeEnd_.setInterval(150);
    connect(&resizeEnd_, &QTimer::timeout, this, [this] {
        const auto [width, height] = size();
        publish(
            "winhelper.onsizeWindowDone",
            {QJsonObject{
                {"left", 0},
                {"top", 0},
                {"right", width},
                {"bottom", height},
                {"width", width},
                {"height", height}}});
    });
    connectWindow();
    connectView();
}

WebWindow::~WebWindow()
{
    using enum saucer::window::event;
    for (auto event : {close, focus, resize, minimize, maximize, decorated})
        window_->off(event);
    using enum saucer::webview::event;
    for (auto event : {message, navigate})
        view_.off(event);
}

bool WebWindow::isVisible() const
{
    return window_->visible();
}

void WebWindow::setVisible(bool visible)
{
    if (visible == isVisible())
        return;
    if (visible)
        window_->show();
    else
        window_->hide();
    publishSizeStatus(visible ? "main" : "hide");
}

void WebWindow::activate()
{
    const bool hidden = !isVisible();
    window_->show();
    if (hidden)
        publishSizeStatus("main");
    if (isMinimized())
        window_->set_minimized(false);
    window_->focus();
}

bool WebWindow::isMinimized() const
{
    return window_->minimized();
}

bool WebWindow::isMaximized() const
{
    return window_->maximized();
}

bool WebWindow::isFullScreen() const
{
    return window_->fullscreen();
}

void WebWindow::showMinimized()
{
    show();
    window_->set_minimized(true);
}

void WebWindow::showMaximized()
{
    show();
    window_->set_maximized(true);
}

void WebWindow::showNormal()
{
    show();
    setFullScreen(false);
    window_->set_minimized(false);
    if (isMaximized())
        window_->set_maximized(false);
}

void WebWindow::setFullScreen(bool fullScreen)
{
    show();
    if (fullScreen != isFullScreen())
        window_->set_fullscreen(fullScreen);
}

void WebWindow::setResizable(bool resizable)
{
    window_->set_resizable(resizable);
}

void WebWindow::setTitle(const QString &title)
{
    window_->set_title(title.toStdString());
}

bool WebWindow::setIcon(const QString &path)
{
    const auto pixmap = QIcon(path).pixmap(256);
    if (pixmap.isNull())
        return false;
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG");
    auto icon = saucer::icon::from(
        saucer::stash::from(std::vector<std::uint8_t>(png.cbegin(), png.cend())));
    if (!icon)
        return false;
    window_->set_icon(*icon);
    return true;
}

WId WebWindow::nativeId() const
{
    return native::windowId(*window_);
}

QSize WebWindow::size() const
{
    const auto [width, height] = window_->size();
    return {width, height};
}

QRect WebWindow::geometry() const
{
    const auto scale = native::scaleFactor(*window_);
    const auto [x, y] = window_->position();
    return {QPoint(qRound(x / scale), qRound(y / scale)), size()};
}

void WebWindow::resize(const QSize &size)
{
    window_->set_size({size.width(), size.height()});
}

void WebWindow::move(const QPoint &position)
{
    const auto scale = native::scaleFactor(*window_);
    window_->set_position({qRound(position.x() * scale), qRound(position.y() * scale)});
}

void WebWindow::setSizeLimits(const QSize &minimum, const QSize &maximum)
{
    window_->set_min_size({minimum.width(), minimum.height()});
    window_->set_max_size({maximum.width(), maximum.height()});
}

void WebWindow::startMove()
{
    window_->start_drag();
}

void WebWindow::startResize(Qt::Edges edges)
{
    using enum saucer::window::edge;
    std::uint8_t bits = 0;
    if (edges & Qt::TopEdge)
        bits |= std::uint8_t(top);
    if (edges & Qt::BottomEdge)
        bits |= std::uint8_t(bottom);
    if (edges & Qt::LeftEdge)
        bits |= std::uint8_t(left);
    if (edges & Qt::RightEdge)
        bits |= std::uint8_t(right);
    if (bits)
        window_->start_resize(saucer::window::edge(bits));
}

QUrl WebWindow::url() const
{
    return toQUrl(view_.url());
}

bool WebWindow::showsAppPage() const
{
    return ResourceHandler::isAppOrigin(url());
}

void WebWindow::load(const QUrl &url)
{
    view_.set_url(url.toString(QUrl::FullyEncoded).toStdString());
}

saucer::status WebWindow::handleMessage(std::string_view raw)
{
    const auto message
        = QJsonDocument::fromJson(QByteArray(raw.data(), qsizetype(raw.size()))).object();
    if (message.value("ncm").toString() != "call")
        return saucer::status::unhandled;
    if (!showsAppPage())
        return saucer::status::handled;

    const auto document = message.value("doc").toString();
    const auto sequence = message.value("seq");
    auto reply = [self = QPointer(this), document, sequence](QJsonArray result) {
        if (self)
            self->post(
                {{"kind", "completed"}, {"doc", document}, {"seq", sequence}, {"args", result}});
    };
    host_.bridge().call(
        {name_, name_ + ':' + document},
        message.value("method").toString(),
        message.value("args").toArray(),
        std::move(reply));
    return saucer::status::handled;
}

void WebWindow::post(const QJsonObject &message)
{
    if (!showsAppPage())
        return;
    const auto json = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
    view_.execute(("window.__ncmHost && window.__ncmHost(" + json + ");").toStdString());
}

void WebWindow::deliverEvent(const QString &target, const QString &name, const QJsonArray &args)
{
    if (!target.isEmpty() && target != name_ && !target.startsWith(name_ + ':'))
        return;
    post({{"kind", "event"}, {"target", target}, {"name", name}, {"args", args}});
}

void WebWindow::publish(const QString &name, const QJsonArray &args)
{
    host_.bridge().publishTo(name_, name, args);
}

void WebWindow::publishSizeStatus(const QString &state)
{
    auto current = state;
    if (current.isEmpty())
        current = isMinimized() ? "minimize" : isMaximized() ? "maximize" : "restore";
    const auto [width, height] = size();
    publish("winhelper.onSizeStatus", {current, width, height});
}

void WebWindow::connectWindow()
{
    using enum saucer::window::event;
    window_->on<close>([this] {
        publish("winhelper.onclose");
        return saucer::policy::block;
    });
    window_->on<focus>(
        [this](bool focused) { publish(focused ? "winhelper.onfocus" : "winhelper.onlosefocus"); });
    window_->on<resize>([this](int, int) {
        publishSizeStatus();
        resizeEnd_.start();
    });
    window_->on<minimize>([this](bool) { publishSizeStatus(); });
    window_->on<maximize>([this](bool) { publishSizeStatus(); });
    window_->on<decorated>([this](saucer::window::decoration decoration) {
        if (decoration == frame_)
            return;
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (!isFullScreen() && window_->decorations() != frame_)
                    window_->set_decorations(frame_);
            },
            Qt::QueuedConnection);
    });
}

void WebWindow::connectView()
{
    using enum saucer::webview::event;
    view_.on<message>([this](std::string_view raw) { return handleMessage(raw); });
    view_.on<navigate>([this](const saucer::navigation &nav) { return handleNavigation(nav); });
    view_.handle_scheme(
        "orpheus", [this](saucer::scheme::request request, saucer::scheme::executor exec) {
            host_.resources().handle(request, exec, showsAppPage());
        });
    native::onFrameNavigation(view_, [this](const QUrl &url, bool redirect) {
        handleFrameNavigation(url, redirect);
    });
}

saucer::policy WebWindow::handleNavigation(const saucer::navigation &nav)
{
    const auto url = toQUrl(nav.url());
    if (nav.new_window()) {
        publish("browser.onbeforepopup", {0, url.toString(), false});
        return saucer::policy::block;
    }
    if (ResourceHandler::isAppOrigin(url) || url.scheme() == "about")
        return saucer::policy::allow;
    if (nav.user_initiated() && (isWeb(url) || url.scheme() == "mailto"))
        QTimer::singleShot(0, this, [url] { QDesktopServices::openUrl(url); });
    return saucer::policy::block;
}

void WebWindow::handleFrameNavigation(const QUrl &url, bool redirect)
{
    if (isWeb(url))
        publish("browser.onbeforerequest", {0, QString(), redirect, url.toString()});
}

}
