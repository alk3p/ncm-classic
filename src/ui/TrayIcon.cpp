#include "TrayIcon.h"

#include "bridge/Bridge.h"
#include "web/ResourceHandler.h"

#include <QCursor>
#include <QUrl>

namespace ncm {

namespace {

#ifdef Q_OS_WIN
constexpr bool menuAttached = false;
#else
constexpr bool menuAttached = true;
#endif

}

TrayIcon::TrayIcon(
    Bridge &bridge,
    const QString &defaultIcon,
    ResourceHandler &resources,
    MenuStyle &style,
    QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , resources_(resources)
    , icon_(QIcon(defaultIcon), this)
    , menu_(style, resources, [this](const QString &id) {
        bridge_.publishTo(owner_, "winhelper.onmenuclick", {id, false});
    })
{
    if (menuAttached) {
        icon_.setContextMenu(&menu_);
        connect(&menu_, &QMenu::aboutToShow, this, &TrayIcon::requestMenu);
    }
    connect(&menu_, &QMenu::aboutToHide, this, [this] {
        bridge_.publishTo(owner_, "winhelper.onmenuhide");
    });
    connect(
        &icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger)
                bridge_.publish("trayicon.onclick");
            else if (reason == QSystemTrayIcon::Context && !menuAttached)
                requestMenu();
        });

    const auto add = [&bridge](const QString &name, Bridge::Handler handler) {
        bridge.addHandler("trayicon." + name, std::move(handler));
    };
    add("install", [this](const QJsonArray &, Bridge::Reply reply) {
        icon_.show();
        reply({icon_.isVisible()});
        if (menuAttached)
            requestMenu();
    });
    add("uninstall", [this](const QJsonArray &, Bridge::Reply reply) {
        icon_.hide();
        reply({});
    });
    add("wasInstall",
        [this](const QJsonArray &, Bridge::Reply reply) { reply({icon_.isVisible()}); });
    add("setToolTip", [this](const QJsonArray &a, Bridge::Reply reply) {
        icon_.setToolTip(stringArg(a, 0));
        reply({});
    });
    add("setIcon", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto source = stringArg(a, 0);
        const QIcon icon(source.startsWith("orpheus:") ? resources_.resolve(QUrl(source)) : source);
        if (!icon.isNull())
            icon_.setIcon(icon);
        reply({!icon.isNull()});
    });
}

void TrayIcon::requestMenu()
{
    bridge_.publish("trayicon.onrightclick");
}

void TrayIcon::setMenu(const QJsonArray &items, const QString &owner)
{
    owner_ = owner;
    menu_.setItems(items);
    if (menuAttached)
        return;
    menu_.popup(QCursor::pos());
    menu_.activateWindow();
}

}
