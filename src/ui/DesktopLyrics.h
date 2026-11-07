#pragma once

#include "Lyrics.h"
#include "LyricsPainter.h"

#include <array>
#include <memory>

#include <QTimer>
#include <QWidget>

namespace ncm {

class Skin;
class SpriteButton;

class DesktopLyrics final : public QWidget
{
    Q_OBJECT

public:
    explicit DesktopLyrics(Skin &, QWidget *parent = nullptr);
    ~DesktopLyrics() override;

    void setLyrics(Lyrics);
    void setMessage(const QString &);
    void setTime(qint64 milliseconds);

    const LyricsLook &look() const { return look_; }
    void setColors(LyricsLook::Gradient base, LyricsLook::Gradient played);
    void setLyricsFont(int pointSize, const QString &family);
    void setSingleLine(bool);
    void setAlignment(Qt::Alignment first, Qt::Alignment second);

    void setLocked(bool);
    void setTopMost(bool);
    void setPlaying(bool);
    void setButtonTips(const QString &previous, const QString &playPause, const QString &next);

signals:
    void action(const QString &name);
    void wheeled(int delta);

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void moveEvent(QMoveEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    class UnlockWindow;

    QWidget *makeToolbar();
    std::array<QRect, 2> rows() const;
    void relayout();
    void setHovered(bool);
    void showUnlock(bool);
    void placeUnlock();
    void pollCursor();
    Qt::Edges resizeEdges(QPoint) const;

    Skin &skin_;
    Lyrics lyrics_;
    QString message_;
    qsizetype line_ = 0;
    double progress_ = 0;
    int sung_ = -1;
    std::array<LyricsLineImage, 2> images_;
    LyricsLook look_;
    bool single_ = true;
    std::array<Qt::Alignment, 2> alignment_{Qt::AlignHCenter, Qt::AlignHCenter};
    bool locked_ = false;
    bool hovered_ = false;

    QWidget *toolbar_;
    SpriteButton *previous_;
    SpriteButton *playPause_;
    SpriteButton *next_;
    std::unique_ptr<UnlockWindow> unlock_;
    QTimer leaveDelay_;
    QTimer cursorPoll_;
};

}
