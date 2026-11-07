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
    return g_application_register(G_APPLICATION(app.native().application), nullptr, nullptr);
#else
    (void) app;
    return true;
#endif
}

WId windowId(const saucer::window &)
{
    return 0;
}

double scaleFactor(const saucer::window &)
{
    return 1;
}

void setCookie(saucer::webview &, const QNetworkCookie &) {}

void removeCookie(saucer::webview &, const QNetworkCookie &) {}

void onFrameNavigation(saucer::webview &, FrameNavigation) {}

}
