#pragma once

#include <QObject>
#include <QPointer>

namespace ncm {

class Bridge;
class MenuStyle;
class PageMenu;
class ResourceHandler;
class TrayIcon;

class PopupMenus final : public QObject
{
public:
    PopupMenus(Bridge &, ResourceHandler &, MenuStyle &, TrayIcon &, QObject *parent = nullptr);

private:
    QPointer<PageMenu> shown_;
};

}
