#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QPixmap>

namespace ncm {

class Skin;

class SpriteButton final : public QAbstractButton
{
public:
    struct Sprite
    {
        QString file;
        QString hoverFile;
        QRect normal;
        QRect hover;
        QRect pressed;
        QRect disabled;
        QSize size;
        QPoint offset;
        QColor tint;
    };

    SpriteButton(Skin &, Sprite, const QString &toolTip, QWidget *parent = nullptr);

    void setSprite(Sprite);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void loadImages();
    QRect source() const;

    Skin &skin_;
    Sprite sprite_;
    QPixmap sheet_;
    QPixmap hoverSheet_;
};

}
