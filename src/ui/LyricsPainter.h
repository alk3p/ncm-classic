#pragma once

#include <array>

#include <QColor>
#include <QFont>
#include <QPixmap>
#include <QRect>
#include <QString>

class QPainter;

namespace ncm {

struct LyricsLook
{
    using Gradient = std::array<QColor, 2>;

    QFont font;
    Gradient base{QColor(0x49, 0x49, 0x4d), QColor(0xa6, 0xa5, 0xaa)};
    Gradient played{QColor(0xd1, 0x0b, 0x0b), QColor(0xfc, 0x78, 0x78)};
    QColor outline{0, 0, 0, 0x3f};

    bool operator==(const LyricsLook &) const = default;
};

class LyricsLineImage
{
public:
    void paint(
        QPainter &,
        const QRect &box,
        Qt::Alignment,
        const QString &text,
        double progress,
        const LyricsLook &);

private:
    void render(const QString &text, const LyricsLook &, int height, qreal pixelRatio);

    QString text_;
    LyricsLook look_;
    int height_ = 0;
    qreal pixelRatio_ = 0;
    QSize size_;
    QRectF glyphs_;
    QPixmap unsung_;
    QPixmap sung_;
};

void paintLyricsLine(
    QPainter &,
    const QRect &box,
    Qt::Alignment,
    const QString &text,
    double progress,
    const LyricsLook &);

int lyricsLineHeight(const LyricsLook &);

}
