#include "MenuStyle.h"

#include "Skin.h"

#include <algorithm>

#include <QMenu>
#include <QPainter>
#include <QStyleFactory>
#include <QStyleOptionMenuItem>
#include <qdrawutil.h>

namespace ncm {

namespace {

constexpr QMargins panelMargins(8, 6, 8, 10);
constexpr int itemHeight = 28;
constexpr int iconColumn = 40;
constexpr int arrowColumn = 26;
constexpr int markSize = 18;
constexpr int minItemWidth = 148;
constexpr int maxItemWidth = 208;
constexpr int fontPixels = 12;

QColor disabledText()
{
    return {0x99, 0x99, 0x9d};
}

QRect centered(QSize size, const QRect &box)
{
    return {box.center() - QPoint(size.width() / 2, size.height() / 2), size};
}

}

MenuStyle::MenuStyle(Skin &skin)
    : QProxyStyle(QStyleFactory::create("Fusion"))
    , skin_(skin)
{}

void MenuStyle::apply(QMenu *menu)
{
    menu->setStyle(this);
    menu->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground);
    menu->setContentsMargins(panelMargins);
    auto font = menu->font();
    font.setPixelSize(fontPixels);
    menu->setFont(font);
}

void MenuStyle::drawPrimitive(
    PrimitiveElement element,
    const QStyleOption *option,
    QPainter *painter,
    const QWidget *widget) const
{
    switch (element) {
    case PE_PanelMenu:
        qDrawBorderPixmap(painter, option->rect, panelMargins, skin_.pixmap("menu/bk.png"));
        return;
    case PE_FrameMenu:
        return;
    default:
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
}

void MenuStyle::drawControl(
    ControlElement element,
    const QStyleOption *option,
    QPainter *painter,
    const QWidget *widget) const
{
    const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
    if (element != CE_MenuItem || !item) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }
    const auto &box = item->rect;
    if (item->menuItemType == QStyleOptionMenuItem::Separator) {
        painter->drawPixmap(
            box.left(), box.center().y(), box.width(), 1, skin_.pixmap("menu/separator.png"));
        return;
    }

    const bool enabled = item->state & State_Enabled;
    const bool selected = enabled && (item->state & State_Selected);
    painter->drawPixmap(box, skin_.pixmap(selected ? "menu/hover.png" : "menu/item.png"));

    const QRect leftColumn(box.left(), box.top(), iconColumn, box.height());
    if (!item->icon.isNull())
        item->icon.paint(painter, centered({markSize, markSize}, leftColumn));

    const auto tab = item->text.indexOf('\t');
    const auto label = item->text.left(tab);
    const auto shortcut = tab < 0 ? QString() : item->text.mid(tab + 1);
    const auto textBox = box.adjusted(iconColumn, 0, -arrowColumn, 0);
    const QFontMetrics metrics(item->font);
    painter->setFont(item->font);
    painter->setPen(enabled ? skin_.color(Skin::Color::MenuItemText) : disabledText());
    const auto labelWidth = textBox.width()
                            - (shortcut.isEmpty() ? 0 : metrics.horizontalAdvance(shortcut) + 8);
    painter->drawText(
        textBox,
        Qt::AlignLeft | Qt::AlignVCenter,
        metrics.elidedText(label, Qt::ElideRight, labelWidth));
    if (!shortcut.isEmpty())
        painter->drawText(textBox, Qt::AlignRight | Qt::AlignVCenter, shortcut);

    if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
        const QRect rightColumn(box.right() - arrowColumn + 1, box.top(), arrowColumn, box.height());
        painter->drawPixmap(
            centered({markSize, markSize}, rightColumn), skin_.pixmap("menu/sub_icon.png"));
    }
}

QSize MenuStyle::sizeFromContents(
    ContentsType type, const QStyleOption *option, const QSize &contents, const QWidget *widget) const
{
    const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
    if (type != CT_MenuItem || !item)
        return QProxyStyle::sizeFromContents(type, option, contents, widget);
    if (item->menuItemType == QStyleOptionMenuItem::Separator)
        return {minItemWidth, 1};
    auto text = item->text;
    text.replace('\t', QStringLiteral("        "));
    const auto width = iconColumn + QFontMetrics(item->font).horizontalAdvance(text) + arrowColumn;
    return {std::clamp(width, minItemWidth, maxItemWidth), itemHeight};
}

int MenuStyle::pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const
{
    switch (metric) {
    case PM_MenuPanelWidth:
    case PM_MenuHMargin:
    case PM_MenuVMargin:
    case PM_MenuDesktopFrameWidth:
        return 0;
    default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

}
