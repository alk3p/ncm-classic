#pragma once

#include <QProxyStyle>

class QMenu;

namespace ncm {

class Skin;

class MenuStyle final : public QProxyStyle
{
public:
    explicit MenuStyle(Skin &);

    void apply(QMenu *);

    void drawPrimitive(
        PrimitiveElement, const QStyleOption *, QPainter *, const QWidget *) const override;
    void drawControl(
        ControlElement, const QStyleOption *, QPainter *, const QWidget *) const override;
    QSize sizeFromContents(
        ContentsType, const QStyleOption *, const QSize &, const QWidget *) const override;
    int pixelMetric(PixelMetric, const QStyleOption *, const QWidget *) const override;

private:
    Skin &skin_;
};

}
