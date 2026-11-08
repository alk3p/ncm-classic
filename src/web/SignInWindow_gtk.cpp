#include "SignInWindow.h"

#if defined(SAUCER_WEBKITGTK)

#include <utility>

#include <QDateTime>

#pragma push_macro("signals")
#undef signals
#include <saucer/gtk.window.impl.hpp>
#include <saucer/modules/stable/webkitgtk.hpp>
#pragma pop_macro("signals")

namespace ncm {

namespace {

class WebKitGtkSignIn final : public SignInWindow
{
public:
    WebKitGtkSignIn(
        std::shared_ptr<saucer::window> window,
        saucer::window &owner,
        WebKitWebView *view,
        WebKitGtkSignIn *opener = nullptr)
        : SignInWindow(std::move(window), opener)
        , view_(view)
    {
        gtk_window_set_transient_for(window_->native().window, owner.native().window);
        gtk_widget_set_hexpand(GTK_WIDGET(view_), true);
        gtk_widget_set_vexpand(GTK_WIDGET(view_), true);
        window_->native<false>()->platform->add_widget(GTK_WIDGET(view_));

        const auto create = +[](WebKitWebView *, WebKitNavigationAction *action, gpointer data) {
            return static_cast<WebKitGtkSignIn *>(data)->createPopup(action);
        };
        const auto location = +[](WebKitWebView *view, GParamSpec *, gpointer data) {
            static_cast<WebKitGtkSignIn *>(data)->showUrl(
                QUrl(QString::fromUtf8(webkit_web_view_get_uri(view))));
        };
        g_signal_connect(view_, "create", G_CALLBACK(create), this);
        g_signal_connect(view_, "notify::uri", G_CALLBACK(location), this);
        if (opener) {
            const auto show = +[](WebKitWebView *, gpointer data) {
                static_cast<WebKitGtkSignIn *>(data)->activate();
            };
            const auto close = +[](WebKitWebView *, gpointer data) {
                auto *self = static_cast<WebKitGtkSignIn *>(data);
                QTimer::singleShot(0, self, [self] { self->dismiss(); });
            };
            g_signal_connect(view_, "ready-to-show", G_CALLBACK(show), this);
            g_signal_connect(view_, "close", G_CALLBACK(close), this);
        }
    }

    ~WebKitGtkSignIn() override
    {
        g_signal_handlers_disconnect_by_data(view_, this);
        webkit_web_view_stop_loading(view_);
        window_->native<false>()->platform->remove_widget(GTK_WIDGET(view_));
    }

    void start(NativeNetwork &network)
    {
        webkit_web_view_load_uri(view_, loginUrl().toEncoded().constData());
        watchSession(network);
    }

private:
    GtkWidget *createPopup(WebKitNavigationAction *action)
    {
        if (closing() || !webkit_navigation_action_is_user_gesture(action))
            return nullptr;
        auto window = makeWindow(*window_, true);
        if (!window)
            return nullptr;
        auto *view = WEBKIT_WEB_VIEW(
            g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", view_, nullptr));
        new WebKitGtkSignIn(std::move(window), *window_, view, this);
        return GTK_WIDGET(view);
    }

    void readCookies(CookiesReply reply) override
    {
        auto *manager = webkit_network_session_get_cookie_manager(
            webkit_web_view_get_network_session(view_));
        const auto completed = +[](GObject *source, GAsyncResult *result, gpointer data) {
            const std::unique_ptr<CookiesReply> done(static_cast<CookiesReply *>(data));
            auto *list = webkit_cookie_manager_get_cookies_finish(
                WEBKIT_COOKIE_MANAGER(source), result, nullptr);
            QList<QNetworkCookie> cookies;
            for (auto *item = list; item; item = item->next) {
                auto *source = static_cast<SoupCookie *>(item->data);
                const QByteArray name(soup_cookie_get_name(source));
                if (name != "MUSIC_U" && name != "__csrf")
                    continue;
                QNetworkCookie cookie(name, soup_cookie_get_value(source));
                cookie.setDomain(QString::fromUtf8(soup_cookie_get_domain(source)));
                cookie.setPath(QString::fromUtf8(soup_cookie_get_path(source)));
                cookie.setHttpOnly(soup_cookie_get_http_only(source));
                if (auto *expires = soup_cookie_get_expires(source))
                    cookie.setExpirationDate(
                        QDateTime::fromSecsSinceEpoch(g_date_time_to_unix(expires), Qt::UTC));
                cookies.append(cookie);
            }
            g_list_free_full(list, +[](gpointer item) {
                soup_cookie_free(static_cast<SoupCookie *>(item));
            });
            (*done)(std::move(cookies));
        };
        webkit_cookie_manager_get_cookies(
            manager,
            "https://music.163.com/",
            nullptr,
            completed,
            new CookiesReply(std::move(reply)));
    }

    WebKitWebView *view_;
};

}

SignInWindow *SignInWindow::create(saucer::webview &owner, NativeNetwork &network)
{
    auto window = makeWindow(owner.parent());
    if (!window)
        return nullptr;
    auto *session = webkit_network_session_new_ephemeral();
    auto *view = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session, nullptr));
    g_object_unref(session);
    auto *signIn = new WebKitGtkSignIn(std::move(window), owner.parent(), view);
    signIn->start(network);
    return signIn;
}

}

#endif
