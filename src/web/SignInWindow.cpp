#include "SignInWindow.h"

#include "services/network/NativeNetwork.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <QDebug>
#include <QPointer>

namespace ncm {

using namespace std::chrono_literals;

#if !defined(SAUCER_WEBVIEW2) && !defined(SAUCER_WEBKITGTK)
SignInWindow *SignInWindow::create(saucer::webview &, NativeNetwork &)
{
    return nullptr;
}
#endif

SignInWindow::SignInWindow(std::shared_ptr<saucer::window> window, SignInWindow *opener)
    : QObject(opener)
    , window_(std::move(window))
{
    showUrl(opener ? QUrl("about:blank") : loginUrl());
    window_->on<saucer::window::event::close>([this] {
        emit closeRequested();
        QTimer::singleShot(0, this, &SignInWindow::dismiss);
        return saucer::policy::block;
    });
    poll_.setSingleShot(true);
}

SignInWindow::~SignInWindow()
{
    for (auto *popup : findChildren<SignInWindow *>(Qt::FindDirectChildrenOnly))
        delete popup;
    window_->off(saucer::window::event::close);
}

const QUrl &SignInWindow::loginUrl()
{
    static const QUrl url("https://music.163.com/#/login");
    return url;
}

std::shared_ptr<saucer::window> SignInWindow::makeWindow(saucer::window &owner, bool popup)
{
    auto window = saucer::window::create(std::addressof(owner.parent()));
    if (!window) {
        qWarning().noquote() << "Cannot create sign-in window:"
                             << QString::fromStdString(window.error().message());
        return {};
    }
    (*window)->set_decorations(saucer::window::decoration::full);
    (*window)->set_size(popup ? saucer::size{800, 600} : saucer::size{1024, 576});
    return *window;
}

void SignInWindow::activate()
{
    if (closing_)
        return;
    window_->show();
    window_->set_minimized(false);
    window_->focus();
}

void SignInWindow::dismiss()
{
    if (closing_)
        return;
    closing_ = true;
    poll_.stop();
    attempt_.reset();
    for (auto *popup : findChildren<SignInWindow *>(Qt::FindDirectChildrenOnly))
        popup->dismiss();
    window_->hide();
    deleteLater();
}

void SignInWindow::showUrl(const QUrl &url)
{
    const auto displayUrl = url == loginUrl()
                                ? url
                                : url.adjusted(
                                      QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
    if (!displayUrl.host().isEmpty() || displayUrl == QUrl("about:blank"))
        window_->set_title(displayUrl.toString(QUrl::FullyEncoded).toStdString());
}

void SignInWindow::watchSession(NativeNetwork &network)
{
    attempt_ = std::make_unique<QObject>();
    connect(&poll_, &QTimer::timeout, this, [this, &network] { checkSession(network); });
    poll_.start(1s);
}

void SignInWindow::checkSession(NativeNetwork &network)
{
    readCookies([self = QPointer(this), &network](QList<QNetworkCookie> cookies) {
        if (!self || !self->attempt_)
            return;
        if (std::ranges::none_of(cookies, [](const auto &c) { return c.name() == "MUSIC_U"; })) {
            self->warning_.clear();
            self->poll_.start(1s);
            return;
        }
        network.importSession(std::move(cookies), self->attempt_.get(), [self](auto result) {
            if (result) {
                self->dismiss();
                return;
            }
            if (self->warning_ != result.error()) {
                self->warning_ = result.error();
                emit self->warning(self->warning_);
            }
            self->poll_.start(5s);
        });
    });
}

}
