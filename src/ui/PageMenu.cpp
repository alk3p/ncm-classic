#include "PageMenu.h"

#include "MenuStyle.h"
#include "bridge/JsonArgs.h"
#include "web/ResourceHandler.h"

namespace ncm {

PageMenu::PageMenu(
    MenuStyle &style, const ResourceHandler &resources, Triggered triggered, QWidget *parent)
    : QMenu(parent)
    , style_(style)
    , resources_(resources)
    , triggered_(std::move(triggered))
{
    style_.apply(this);
}

void PageMenu::setItems(const QJsonArray &items)
{
    clear();
    qDeleteAll(findChildren<QMenu *>(Qt::FindDirectChildrenOnly));
    fill(*this, items);
}

QIcon PageMenu::icon(const QJsonObject &item) const
{
    for (const auto key : {"check_image_path", "image_path"})
        if (const auto url = item[key].toString(); !url.isEmpty())
            return QIcon(resources_.resolve(QUrl(url)));
    return {};
}

void PageMenu::fill(QMenu &menu, const QJsonArray &items)
{
    for (const auto &value : items) {
        const auto item = jsonObject(value);
        if (item.contains("menu") && !item["menu"].toBool())
            continue;
        if (item["separator"].toBool()) {
            menu.addSeparator();
            continue;
        }
        const auto text = item["text"].toString();
        const bool enabled = !item.contains("enable") || item["enable"].toBool();
        if (const auto children = item["children"]; children.isArray()) {
            auto *submenu = new QMenu(text, this);
            style_.apply(submenu);
            submenu->setIcon(icon(item));
            submenu->setEnabled(enabled);
            fill(*submenu, children.toArray());
            menu.addMenu(submenu);
            continue;
        }
        auto *action = menu.addAction(icon(item), text);
        action->setEnabled(enabled);
        connect(action, &QAction::triggered, this, [this, id = item["menu_id"].toVariant().toString()] {
            triggered_(id);
        });
    }
}

}
