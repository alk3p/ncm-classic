#include "DesktopIntegration.h"

#include "bridge/Bridge.h"

namespace ncm {

void DesktopIntegration::registerAutostart()
{
    for (const auto name : {"app.getAutoRunState", "app.setAutoRun"})
        bridge_.addHandler(name, [](const QJsonArray &, Bridge::Reply reply) { reply({false}); });
    bridge_.addHandler("app.cancelAutoRun", [](const QJsonArray &, Bridge::Reply reply) {
        reply({true});
    });
}

void DesktopIntegration::registerDefaultClient()
{
    for (const auto name : {"app.isRegisterDefaultClient", "app.registerDefaultClient"})
        bridge_.addHandler(name, [](const QJsonArray &, Bridge::Reply reply) { reply({false}); });
    bridge_.addHandler("app.unRegisterDefaultClient", [](const QJsonArray &, Bridge::Reply reply) {
        reply({true});
    });
}

}
