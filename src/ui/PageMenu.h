#pragma once

#include <functional>

#include <QJsonArray>
#include <QMenu>

namespace ncm {

class MenuStyle;
class ResourceHandler;

class PageMenu final : public QMenu
{
public:
    using Triggered = std::function<void(const QString &menuId)>;

    PageMenu(MenuStyle &, const ResourceHandler &, Triggered, QWidget *parent = nullptr);

    void setItems(const QJsonArray &items);

private:
    void fill(QMenu &, const QJsonArray &items);
    QIcon icon(const QJsonObject &item) const;

    MenuStyle &style_;
    const ResourceHandler &resources_;
    Triggered triggered_;
};

}
