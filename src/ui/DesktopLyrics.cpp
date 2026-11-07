#include "DesktopLyrics.h"

#include "Skin.h"
#include "SpriteButton.h"
#include "WindowFlags.h"

#include <algorithm>
#include <functional>

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QWindow>
#include <qdrawutil.h>

namespace ncm {

namespace {

constexpr int defaultWidth = 662;
constexpr int minWidth = 542;
constexpr QMargins frameMargins(6, 6, 6, 6);
constexpr int toolbarTop = 6 + 5;
constexpr QSize toolbarSize(310, 20);
constexpr int linesTop = toolbarTop + 20 + 5;
constexpr int linesBottom = 41 - linesTop;
constexpr int textInset = 12;
constexpr int resizeBorder = 6;
constexpr int defaultFontPoints = 36;
constexpr int leaveDelayMs = 400;
constexpr int cursorPollMs = 200;

enum Column {
    Detail = 0,
    Previous = 20,
    Play = 40,
    Next = 60,
    Setting = 80,
    Lock = 100,
    Back = 120,
    Forward = 140,
    Close = 160,
    Pause = 180
};

SpriteButton::Sprite buttonSprite(int column)
{
    return {
        .file = "lrc/button.png",
        .normal = {column, 0, 20, 20},
        .hover = {column, 20, 20, 20},
        .pressed = {column, 40, 20, 20},
        .disabled = {column, 60, 20, 20},
    };
}

}

class DesktopLyrics::UnlockWindow final : public QWidget
{
public:
    UnlockWindow(Skin &skin, std::function<void()> clicked)
        : QWidget(
              nullptr,
              Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                  | Qt::NoDropShadowWindowHint)
        , skin_(skin)
        , clicked_(std::move(clicked))
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_Hover);
        setFixedSize(40, 36);
        setCursor(Qt::PointingHandCursor);
        setToolTip(tr("解锁桌面歌词"));
    }

    void setRevealed(bool revealed)
    {
        if (revealed == revealed_)
            return;
        revealed_ = revealed;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        if (!revealed_ && !underMouse()) {
            painter.fillRect(rect(), QColor(0, 0, 0, 1));
            return;
        }
        painter.drawPixmap(0, 0, skin_.pixmap("lrc/desk_btn_unlock.png"));
        const auto icon = skin_.pixmap(
            underMouse() ? "lrc/desk_icn_unlock_hvr.png" : "lrc/desk_icn_unlock.png");
        painter.drawPixmap((width() - icon.width()) / 2, (height() - icon.height()) / 2, icon);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
            clicked_();
    }

private:
    Skin &skin_;
    std::function<void()> clicked_;
    bool revealed_ = false;
};

DesktopLyrics::DesktopLyrics(Skin &skin, QWidget *parent)
    : QWidget(
          parent,
          Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint)
    , skin_(skin)
    , message_(tr("网易云音乐  听见好时光"))
    , unlock_(std::make_unique<UnlockWindow>(skin, [this] { emit action("unlock"); }))
{
    setWindowTitle(tr("桌面歌词"));
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setMinimumWidth(minWidth);
    resize(defaultWidth, height());
    look_.font.setPointSize(defaultFontPoints);
    look_.outline = skin_.color(Skin::Color::LyricsOutline);

    toolbar_ = makeToolbar();
    toolbar_->hide();
    relayout();

    leaveDelay_.setSingleShot(true);
    leaveDelay_.setInterval(leaveDelayMs);
    connect(&leaveDelay_, &QTimer::timeout, this, [this] { setHovered(false); });
    cursorPoll_.setInterval(cursorPollMs);
    connect(&cursorPoll_, &QTimer::timeout, this, &DesktopLyrics::pollCursor);
    connect(&skin_, &Skin::changed, this, [this] {
        look_.outline = skin_.color(Skin::Color::LyricsOutline);
        update();
    });
}

DesktopLyrics::~DesktopLyrics() = default;

QWidget *DesktopLyrics::makeToolbar()
{
    auto *toolbar = new QWidget(this);
    toolbar->setFixedSize(toolbarSize);
    auto *layout = new QHBoxLayout(toolbar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    const auto button = [&](int column, const QString &tip, const char *name) {
        auto *b = new SpriteButton(skin_, buttonSprite(column), tip, toolbar);
        connect(b, &QAbstractButton::clicked, this, [this, name] { emit action(name); });
        layout->addWidget(b);
        return b;
    };
    layout->addSpacing(6);
    button(Detail, tr("进入音乐详情页"), "detail");
    layout->addSpacing(7 + 2 + 11);
    previous_ = button(Previous, tr("上一首"), "playprev");
    layout->addSpacing(11);
    playPause_ = button(Play, tr("播放"), "play_pause");
    layout->addSpacing(11);
    next_ = button(Next, tr("下一首"), "playnext");
    layout->addSpacing(8 + 2 + 9);
    button(Setting, tr("设置"), "setting");
    layout->addSpacing(7);
    button(Lock, tr("锁定桌面歌词"), "lock");
    layout->addSpacing(7 + 2 + 9);
    button(Back, tr("歌词后退0.5s"), "offset_back");
    layout->addSpacing(11);
    button(Forward, tr("歌词前进0.5s"), "offset_forward");
    layout->addSpacing(10 + 2 + 7);
    button(Close, tr("关闭桌面歌词"), "close");
    layout->addStretch();
    return toolbar;
}

void DesktopLyrics::setLyrics(Lyrics lyrics)
{
    lyrics_ = std::move(lyrics);
    line_ = 0;
    progress_ = 0;
    sung_ = -1;
    update();
}

void DesktopLyrics::setMessage(const QString &message)
{
    lyrics_ = {};
    message_ = message;
    update();
}

void DesktopLyrics::setTime(qint64 milliseconds)
{
    if (lyrics_.isEmpty() || !isVisible())
        return;
    const auto current = lyrics_.lineAt(milliseconds);
    const auto line = std::max<qsizetype>(current, 0);
    const QFontMetrics metrics(look_.font);
    const auto progress = current < 0 ? 0. : lyrics_.progress(line, milliseconds, metrics);
    const int sung = qRound(progress * metrics.horizontalAdvance(lyrics_.line(line).text));
    if (line == line_ && sung == sung_)
        return;
    line_ = line;
    progress_ = progress;
    sung_ = sung;
    update();
}

void DesktopLyrics::setColors(LyricsLook::Gradient base, LyricsLook::Gradient played)
{
    look_.base = base;
    look_.played = played;
    update();
}

void DesktopLyrics::setLyricsFont(int pointSize, const QString &family)
{
    QFont font(family);
    font.setPointSize(std::clamp(pointSize, 12, 96));
    look_.font = font;
    relayout();
}

void DesktopLyrics::setSingleLine(bool single)
{
    single_ = single;
    relayout();
}

void DesktopLyrics::setAlignment(Qt::Alignment first, Qt::Alignment second)
{
    alignment_ = {first, second};
    update();
}

void DesktopLyrics::setLocked(bool locked)
{
    if (locked == locked_)
        return;
    locked_ = locked;
    setHovered(false);
    setWindowFlagInPlace(*this, Qt::WindowTransparentForInput, locked);
    showUnlock(locked && isVisible());
    update();
}

void DesktopLyrics::setTopMost(bool topMost)
{
    setWindowFlagInPlace(*this, Qt::WindowStaysOnTopHint, topMost);
}

void DesktopLyrics::setPlaying(bool playing)
{
    playPause_->setSprite(buttonSprite(playing ? Pause : Play));
}

void DesktopLyrics::setButtonTips(
    const QString &previous, const QString &playPause, const QString &next)
{
    previous_->setToolTip(previous);
    playPause_->setToolTip(playPause);
    next_->setToolTip(next);
}

std::array<QRect, 2> DesktopLyrics::rows() const
{
    const int height = lyricsLineHeight(look_);
    const QRect first(textInset, linesTop, width() - 2 * textInset, height);
    return {first, first.translated(0, height)};
}

void DesktopLyrics::relayout()
{
    setFixedHeight(linesTop + (single_ ? 1 : 2) * lyricsLineHeight(look_) + linesBottom);
    update();
}

void DesktopLyrics::resizeEvent(QResizeEvent *)
{
    toolbar_->move((width() - toolbarSize.width()) / 2, toolbarTop);
    placeUnlock();
}

void DesktopLyrics::moveEvent(QMoveEvent *)
{
    placeUnlock();
}

void DesktopLyrics::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    if (hovered_)
        qDrawBorderPixmap(&painter, rect(), frameMargins, skin_.pixmap("lrc/frame.png"));
    else if (!locked_)
        painter.fillRect(rect(), QColor(0, 0, 0, 1));

    const auto boxes = rows();
    if (lyrics_.isEmpty()) {
        images_[0].paint(painter, boxes[0], alignment_[0], message_, 1, look_);
        return;
    }
    const auto row = single_ ? 0 : line_ % 2;
    const auto &text = lyrics_.line(line_).text;
    images_[row].paint(painter, boxes[row], alignment_[row], text, progress_, look_);
    if (!single_ && line_ + 1 < lyrics_.size()) {
        const auto &next = lyrics_.line(line_ + 1).text;
        images_[1 - row].paint(painter, boxes[1 - row], alignment_[1 - row], next, 0, look_);
    }
}

void DesktopLyrics::setHovered(bool hovered)
{
    hovered_ = hovered && !locked_;
    toolbar_->setVisible(hovered_);
    update();
}

void DesktopLyrics::enterEvent(QEnterEvent *)
{
    leaveDelay_.stop();
    setHovered(true);
}

void DesktopLyrics::leaveEvent(QEvent *)
{
    leaveDelay_.start();
}

Qt::Edges DesktopLyrics::resizeEdges(QPoint position) const
{
    if (position.x() < resizeBorder)
        return Qt::LeftEdge;
    if (position.x() >= width() - resizeBorder)
        return Qt::RightEdge;
    return {};
}

void DesktopLyrics::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !windowHandle())
        return;
    if (const auto edges = resizeEdges(event->position().toPoint()))
        windowHandle()->startSystemResize(edges);
    else
        windowHandle()->startSystemMove();
}

void DesktopLyrics::mouseMoveEvent(QMouseEvent *event)
{
    setCursor(resizeEdges(event->position().toPoint()) ? Qt::SizeHorCursor : Qt::ArrowCursor);
}

void DesktopLyrics::wheelEvent(QWheelEvent *event)
{
    emit wheeled(event->angleDelta().y());
}

void DesktopLyrics::showEvent(QShowEvent *)
{
    showUnlock(locked_);
}

void DesktopLyrics::hideEvent(QHideEvent *)
{
    leaveDelay_.stop();
    setHovered(false);
    showUnlock(false);
}

void DesktopLyrics::showUnlock(bool show)
{
    if (!show) {
        cursorPoll_.stop();
        unlock_->hide();
        return;
    }
    placeUnlock();
    unlock_->setRevealed(false);
    unlock_->show();
    cursorPoll_.start();
}

void DesktopLyrics::placeUnlock()
{
    unlock_->move(x() + (width() - unlock_->width()) / 2, y() + frameMargins.top());
}

void DesktopLyrics::pollCursor()
{
    const auto cursor = QCursor::pos();
    unlock_->setRevealed(frameGeometry().contains(cursor) || unlock_->geometry().contains(cursor));
}

}
