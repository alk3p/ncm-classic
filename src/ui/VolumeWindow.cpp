#include "VolumeWindow.h"

#include "Skin.h"

#include <algorithm>

#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

namespace ncm {

namespace {

constexpr QSize windowSize(124, 48);
constexpr QSize thumbSize(32, 22);
constexpr QRect railAbove(2, 6, 120, 22);
constexpr QRect railBelow(2, 13, 120, 22);
constexpr double wheelStep = 0.05;

}

VolumeWindow::VolumeWindow(Skin &skin, QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , skin_(skin)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setFixedSize(windowSize);
    connect(&skin_, &Skin::changed, this, qOverload<>(&QWidget::update));
}

void VolumeWindow::setVolume(double volume)
{
    volume_ = std::clamp(volume, 0., 1.);
    update();
}

void VolumeWindow::popup(const QRect &anchor)
{
    const auto *screen = QGuiApplication::screenAt(anchor.center());
    const auto available = screen ? screen->availableGeometry() : QRect();
    below_ = anchor.top() - height() < available.top();
    move(anchor.center().x() - width() / 2, below_ ? anchor.bottom() : anchor.top() - height());
    show();
}

QRect VolumeWindow::rail() const
{
    return below_ ? railBelow : railAbove;
}

QRect VolumeWindow::thumb() const
{
    const auto track = rail();
    const int left = track.left() + qRound(volume_ * (track.width() - thumbSize.width()));
    return {QPoint(left, track.top()), thumbSize};
}

void VolumeWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.drawPixmap(
        0,
        0,
        skin_.pixmap(below_ ? "mini/volume/background_bottom.png" : "mini/volume/background.png"));
    const auto track = rail();
    const auto knob = thumb();
    const int filled = knob.center().x() - track.left();
    painter.drawPixmap(
        QRect(track.topLeft(), QSize(filled, track.height())),
        skin_.pixmap("mini/volume/rail.png"),
        QRect(0, 0, filled, track.height()));
    const bool hovered = knob.contains(mapFromGlobal(QCursor::pos()));
    painter.drawPixmap(
        knob.topLeft(), skin_.pixmap(hovered ? "mini/volume/hover.png" : "mini/volume/normal.png"));
}

void VolumeWindow::mousePressEvent(QMouseEvent *event)
{
    if (!rect().contains(event->position().toPoint())) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton)
        drag(event->position().toPoint().x());
}

void VolumeWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton)
        drag(event->position().toPoint().x());
    else
        update();
}

void VolumeWindow::wheelEvent(QWheelEvent *event)
{
    const auto notches = event->angleDelta().y() / 120.;
    const auto volume = std::clamp(volume_ + notches * wheelStep, 0., 1.);
    if (volume == volume_)
        return;
    setVolume(volume);
    emit volumeChanged(volume_);
}

void VolumeWindow::drag(int x)
{
    const auto track = rail();
    const auto volume = std::clamp(
        double(x - track.left() - thumbSize.width() / 2) / (track.width() - thumbSize.width()),
        0.,
        1.);
    if (volume == volume_)
        return;
    setVolume(volume);
    emit volumeChanged(volume_);
}

}
