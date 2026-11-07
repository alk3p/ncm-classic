#pragma once

#include <QWidget>

namespace ncm {

class Skin;

class VolumeWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit VolumeWindow(Skin &, QWidget *parent = nullptr);

    double volume() const { return volume_; }
    void setVolume(double);
    void popup(const QRect &anchor);

signals:
    void volumeChanged(double);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    QRect rail() const;
    QRect thumb() const;
    void drag(int x);

    Skin &skin_;
    double volume_ = 1;
    bool below_ = false;
};

}
