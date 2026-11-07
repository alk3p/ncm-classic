#include "NativeView.h"

#include <QDateTime>

#include <saucer/modules/stable/webview2.hpp>
#include <wrl.h>

namespace ncm::native {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

std::wstring wide(const QString &s)
{
    return s.toStdWString();
}

std::wstring cookiePath(const QNetworkCookie &c)
{
    return wide(c.path().isEmpty() ? QStringLiteral("/") : c.path());
}

ComPtr<ICoreWebView2CookieManager> cookieManager(saucer::webview &view)
{
    ComPtr<ICoreWebView2CookieManager> manager;
    if (auto *core = view.native().webview)
        core->get_CookieManager(&manager);
    return manager;
}

}

bool attachToQtLoop(saucer::application &)
{
    return true;
}

WId windowId(const saucer::window &window)
{
    return WId(window.native().hwnd);
}

double scaleFactor(const saucer::window &window)
{
    const auto dpi = GetDpiForWindow(window.native().hwnd);
    return dpi ? dpi / double(USER_DEFAULT_SCREEN_DPI) : 1;
}

void setCookie(saucer::webview &view, const QNetworkCookie &c)
{
    const auto manager = cookieManager(view);
    ComPtr<ICoreWebView2Cookie> cookie;
    if (!manager
        || FAILED(manager->CreateCookie(
            wide(QString::fromUtf8(c.name())).c_str(),
            wide(QString::fromUtf8(c.value())).c_str(),
            wide(c.domain()).c_str(),
            cookiePath(c).c_str(),
            &cookie)))
        return;
    cookie->put_IsSecure(c.isSecure());
    cookie->put_IsHttpOnly(c.isHttpOnly());
    if (!c.isSessionCookie())
        cookie->put_Expires(double(c.expirationDate().toSecsSinceEpoch()));
    switch (c.sameSitePolicy()) {
    case QNetworkCookie::SameSite::Strict:
        cookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT);
        break;
    case QNetworkCookie::SameSite::Lax:
        cookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX);
        break;
    case QNetworkCookie::SameSite::None:
        cookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE);
        break;
    case QNetworkCookie::SameSite::Default:
        break;
    }
    manager->AddOrUpdateCookie(cookie.Get());
}

void removeCookie(saucer::webview &view, const QNetworkCookie &c)
{
    if (const auto manager = cookieManager(view))
        manager->DeleteCookiesWithDomainAndPath(
            wide(QString::fromUtf8(c.name())).c_str(),
            wide(c.domain()).c_str(),
            cookiePath(c).c_str());
}

void onFrameNavigation(saucer::webview &view, FrameNavigation handler)
{
    auto *core = view.native().webview;
    if (!core)
        return;
    auto callback = [handler = std::move(
                         handler)](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) {
        LPWSTR raw = nullptr;
        if (FAILED(args->get_Uri(&raw)))
            return S_OK;
        const QUrl url(QString::fromWCharArray(raw));
        CoTaskMemFree(raw);
        BOOL redirected = FALSE;
        args->get_IsRedirected(&redirected);
        handler(url, redirected);
        return S_OK;
    };
    EventRegistrationToken token;
    core->add_FrameNavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(std::move(callback)).Get(), &token);
}

}
