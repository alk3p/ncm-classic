#include "DesktopIntegration.h"

#include "bridge/Bridge.h"

#include <QCoreApplication>
#include <QSettings>

namespace ncm {

namespace {

constexpr auto runKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";

}

void DesktopIntegration::registerAutostart()
{
    bridge_.addHandler("app.getAutoRunState", [](const QJsonArray &, Bridge::Reply reply) {
        reply({QSettings(runKey, QSettings::NativeFormat).contains(NCM_APP_ID)});
    });
    bridge_.addHandler("app.setAutoRun", [](const QJsonArray &, Bridge::Reply reply) {
        QSettings run(runKey, QSettings::NativeFormat);
        run.setValue(NCM_APP_ID, '"' + QCoreApplication::applicationFilePath() + '"');
        run.sync();
        reply({run.status() == QSettings::NoError});
    });
    bridge_.addHandler("app.cancelAutoRun", [](const QJsonArray &, Bridge::Reply reply) {
        QSettings run(runKey, QSettings::NativeFormat);
        run.remove(NCM_APP_ID);
        run.sync();
        reply({run.status() == QSettings::NoError});
    });
}

void DesktopIntegration::registerDefaultClient()
{
    bridge_.addHandler("app.isRegisterDefaultClient", [](const QJsonArray &, Bridge::Reply reply) {
        reply({false});
    });
    bridge_.addHandler("app.registerDefaultClient", [](const QJsonArray &, Bridge::Reply reply) {
        reply({false});
    });
    bridge_.addHandler("app.unRegisterDefaultClient", [](const QJsonArray &, Bridge::Reply reply) {
        reply({true});
    });
}

}
