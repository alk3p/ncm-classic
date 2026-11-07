#include "SpriteButton.h"

#include "Skin.h"

#include <QPainter>

namespace ncm {

namespace {

QPixmap tinted(const QPixmap &image, const QColor &color)
{
    if (image.isNull())
        return image;
    QPixmap result(image.size());
    result.setDevicePixelRatio(image.devicePixelRatio());
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.drawPixmap(0, 0, image);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(result.rect(), color);
    return result;
}

}

SpriteButton::SpriteButton(Skin &skin, Sprite sprite, const QString &toolTip, QWidget *parent)
    : QAbstractButton(parent)
    , skin_(skin)
{
    setToolTip(toolTip);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setSprite(std::move(sprite));
    connect(&skin_, &Skin::changed, this, [this] {
        loadImages();
        update();
    });
}

void SpriteButton::setSprite(Sprite sprite)
{
    sprite_ = std::move(sprite);
    loadImages();
    setFixedSize(sizeHint());
    update();
}

void SpriteButton::loadImages()
{
    sheet_ = skin_.pixmap(sprite_.file);
    hoverSheet_ = sprite_.hoverFile.isEmpty() ? sheet_ : skin_.pixmap(sprite_.hoverFile);
    if (sprite_.tint.isValid()) {
        hoverSheet_ = tinted(hoverSheet_, sprite_.tint.lighter(120));
        sheet_ = tinted(sheet_, sprite_.tint);
    }
}

QSize SpriteButton::sizeHint() const
{
    return sprite_.size.isEmpty() ? sprite_.normal.size() : sprite_.size;
}

QRect SpriteButton::source() const
{
    const auto hover = sprite_.hover.isEmpty() ? sprite_.normal : sprite_.hover;
    if (!isEnabled())
        return sprite_.disabled.isEmpty() ? sprite_.normal : sprite_.disabled;
    if (isDown())
        return sprite_.pressed.isEmpty() ? hover : sprite_.pressed;
    return underMouse() ? hover : sprite_.normal;
}

void SpriteButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    if (!isEnabled() && sprite_.disabled.isEmpty())
        painter.setOpacity(0.4);
    const bool hovered = isEnabled() && (isDown() || underMouse());
    const auto from = source();
    painter.drawPixmap(QRect(sprite_.offset, from.size()), hovered ? hoverSheet_ : sheet_, from);
}

}
