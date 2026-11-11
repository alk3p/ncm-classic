#pragma once

#include <functional>

#include <QNetworkCookie>
#include <QUrl>
#include <QWindow>

namespace saucer {
struct application;
struct webview;
struct window;
}

namespace ncm::native {

bool attachToQtLoop(saucer::application &);

WId windowId(const saucer::window &);
double scaleFactor(const saucer::window &);
void styleMainWindow(const saucer::window &);

void setCookie(saucer::webview &, const QNetworkCookie &);
void removeCookie(saucer::webview &, const QNetworkCookie &);

using FrameNavigation = std::function<void(const QUrl &, bool redirect)>;
void onFrameNavigation(saucer::webview &, FrameNavigation);

}
