#pragma once

#include "PageMenu.h"

#include <QObject>
#include <QSystemTrayIcon>

namespace ncm {

class Bridge;
class ResourceHandler;

class TrayIcon final : public QObject
{
public:
    TrayIcon(
        Bridge &,
        const QString &defaultIcon,
        ResourceHandler &,
        MenuStyle &,
        QObject *parent = nullptr);

    void setMenu(const QJsonArray &items, const QString &owner);

private:
    void requestMenu();

    Bridge &bridge_;
    ResourceHandler &resources_;
    QSystemTrayIcon icon_;
    PageMenu menu_;
    QString owner_;
};

}
