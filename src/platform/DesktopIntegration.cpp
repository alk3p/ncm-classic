#include "DesktopIntegration.h"

#include "bridge/Bridge.h"

namespace ncm {

DesktopIntegration::DesktopIntegration(Bridge &bridge)
    : bridge_(bridge)
{
    registerHotkeys();
    registerAutostart();
    registerDefaultClient();
}

void DesktopIntegration::registerHotkeys()
{
    hotkeys_.registered = [this](const QString &name, bool ok) {
        bridge_.publish("winhelper.onRegisterHotkeyResult", {name, true, ok ? 0 : 1});
    };
    hotkeys_.activated = [this](const QString &name) {
        bridge_.publish("winhelper.onHotkey", {name, true});
    };

    bridge_.addHandler("winhelper.registerHotkey", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto name = stringArg(a, 0);
        if (!boolArg(a, 2)) {
            bridge_.publish("winhelper.onRegisterHotkeyResult", {name, false, 0});
            reply({true});
            return;
        }
        QList<int> keys;
        for (const auto key : a.at(1).toArray())
            keys.append(key.toVariant().toInt());
        hotkeys_.add(name, keys);
        reply({});
    });
    bridge_
        .addHandler("winhelper.unregisterHotkey", [this](const QJsonArray &a, Bridge::Reply reply) {
            const auto name = stringArg(a, 0);
            const bool global = boolArg(a, 1);
            if (global)
                hotkeys_.remove(name);
            bridge_.publish("winhelper.onUnregisterHotkeyResult", {name, global, 0});
            reply({});
        });
}

}
