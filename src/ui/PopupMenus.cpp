#include "PopupMenus.h"

#include "PageMenu.h"
#include "TrayIcon.h"
#include "bridge/Bridge.h"
#include "bridge/JsonArgs.h"

#include <QCursor>

namespace ncm {

namespace {

constexpr int trayMenu = 2;

}

PopupMenus::PopupMenus(
    Bridge &bridge, ResourceHandler &resources, MenuStyle &style, TrayIcon &tray, QObject *parent)
    : QObject(parent)
{
    bridge.addHandler("winhelper.popupMenu", [&, this](const QJsonArray &a, Bridge::Reply reply) {
        const auto content = jsonObject(a.at(0)).value("content");
        const auto items = content.isArray()
                               ? content.toArray()
                               : QJsonDocument::fromJson(content.toString().toUtf8()).array();
        const auto owner = bridge.callingDocument();
        if (int(numberArg(a, 1)) == trayMenu) {
            tray.setMenu(items, owner);
            reply({});
            return;
        }
        if (shown_)
            shown_->close();
        auto *menu = new PageMenu(style, resources, [&bridge, owner](const QString &id) {
            bridge.publishTo(owner, "winhelper.onmenuclick", {id, false});
        });
        menu->setItems(items);
        connect(menu, &QMenu::aboutToHide, menu, [&bridge, menu, owner] {
            bridge.publishTo(owner, "winhelper.onmenuhide");
            menu->deleteLater();
        });
        shown_ = menu;
        menu->popup(QCursor::pos());
        reply({});
    });
}

}
