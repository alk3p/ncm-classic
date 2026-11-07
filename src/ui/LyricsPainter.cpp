#include "LyricsPainter.h"

#include <algorithm>

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>

namespace ncm {

namespace {

int outlineWidth(const QFont &font)
{
    return font.pointSize() > 58 ? 2 : 1;
}

QLinearGradient gradient(const LyricsLook::Gradient &colors, qreal top, qreal ascent)
{
    QLinearGradient fill(0, top, 0, top + ascent);
    fill.setColorAt(0.2, colors[0]);
    fill.setColorAt(0.7, colors[1]);
    return fill;
}

}

int lyricsLineHeight(const LyricsLook &look)
{
    return QFontMetrics(look.font).height();
}

void LyricsLineImage::render(
    const QString &text, const LyricsLook &look, int height, qreal pixelRatio)
{
    text_ = text;
    look_ = look;
    height_ = height;
    pixelRatio_ = pixelRatio;

    const int outline = outlineWidth(look.font);
    const QFontMetrics metrics(look.font);
    size_ = {metrics.horizontalAdvance(text) + 2 * outline, height};
    const int top = (height - metrics.height()) / 2;
    QPainterPath path;
    path.addText(outline, top + metrics.ascent(), look.font, text);
    glyphs_ = path.boundingRect();
    QPainterPathStroker stroker;
    stroker.setWidth(outline);
    stroker.setJoinStyle(Qt::RoundJoin);
    const auto edge = stroker.createStroke(path);

    unsung_ = QPixmap(size_ * pixelRatio);
    unsung_.setDevicePixelRatio(pixelRatio);
    unsung_.fill(Qt::transparent);
    QPainter painter(&unsung_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillPath(path, gradient(look.base, top, metrics.ascent()));
    painter.fillPath(edge, look.outline);
    painter.end();
    sung_ = unsung_;
    painter.begin(&sung_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillPath(path, gradient(look.played, top, metrics.ascent()));
}

void LyricsLineImage::paint(
    QPainter &painter,
    const QRect &box,
    Qt::Alignment alignment,
    const QString &text,
    double progress,
    const LyricsLook &look)
{
    if (text.isEmpty())
        return;
    const auto pixelRatio = painter.device()->devicePixelRatio();
    if (text != text_ || box.height() != height_ || pixelRatio != pixelRatio_ || look != look_)
        render(text, look, box.height(), pixelRatio);

    progress = std::clamp(progress, 0., 1.);
    const int width = size_.width();
    int x = box.left();
    if (width > box.width()) {
        const int sung = qRound(glyphs_.left() + glyphs_.width() * progress);
        x -= std::clamp(sung - box.width() / 2, 0, width - box.width());
    } else if (alignment & Qt::AlignHCenter) {
        x += (box.width() - width) / 2;
    } else if (alignment & Qt::AlignRight) {
        x += box.width() - width;
    }

    painter.save();
    painter.setClipRect(box);
    const QPoint at(x, box.top());
    painter.drawPixmap(at, unsung_);
    if (progress > 0) {
        const double edge = progress < 1 ? glyphs_.left() + glyphs_.width() * progress : width;
        painter.setClipRect(QRectF(at, QSizeF(edge, box.height())), Qt::IntersectClip);
        painter.drawPixmap(at, sung_);
    }
    painter.restore();
}

void paintLyricsLine(
    QPainter &painter,
    const QRect &box,
    Qt::Alignment alignment,
    const QString &text,
    double progress,
    const LyricsLook &look)
{
    LyricsLineImage().paint(painter, box, alignment, text, progress, look);
}

}
