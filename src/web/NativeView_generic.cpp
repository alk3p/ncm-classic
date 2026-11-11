#include "NativeView.h"

#include <saucer/webview.hpp>
#if defined(SAUCER_WEBKITGTK)
#pragma push_macro("signals")
#undef signals
#include <saucer/modules/stable/webkitgtk.hpp>
#pragma pop_macro("signals")
#endif

namespace ncm::native {

bool attachToQtLoop(saucer::application &app)
{
#if defined(SAUCER_WEBKITGTK)
    adw_init();
    if (!g_application_register(G_APPLICATION(app.native().application), nullptr, nullptr))
        return false;

    auto *style = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        style,
        "window.ncm-main-window.csd:not(.solid-csd):not(.ssd-frame)"
        ":not(.maximized):not(.fullscreen):not(.tiled)"
        ":not(.tiled-top):not(.tiled-right):not(.tiled-bottom):not(.tiled-left)"
        " { border-radius: 8px; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(style),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(style);
#else
    (void) app;
#endif
    return true;
}

WId windowId(const saucer::window &)
{
    return 0;
}

double scaleFactor(const saucer::window &)
{
    return 1;
}

void styleMainWindow(const saucer::window &window)
{
#if defined(SAUCER_WEBKITGTK)
    gtk_widget_add_css_class(GTK_WIDGET(window.native().window), "ncm-main-window");
#else
    (void) window;
#endif
}

void setCookie(saucer::webview &, const QNetworkCookie &) {}

void removeCookie(saucer::webview &, const QNetworkCookie &) {}

void onFrameNavigation(saucer::webview &, FrameNavigation) {}

}
