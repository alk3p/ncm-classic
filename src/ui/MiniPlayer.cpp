#include "MiniPlayer.h"

#include "MiniPlaylist.h"
#include "Skin.h"
#include "SpriteButton.h"
#include "WindowFlags.h"

#include <algorithm>
#include <utility>

#include <QAbstractButton>
#include <QContextMenuEvent>
#include <QFrame>
#include <QPainter>
#include <QStackedWidget>
#include <QWindow>

namespace ncm {

namespace {

constexpr QSize barSize(303, 48);
constexpr QRect coverRect(3, 2, 42, 42);
constexpr int middleLeft = 48;
constexpr QRect gradientRect(200, 2, 100, 42);
constexpr QRect playlistRect(0, 44, barSize.width(), 285);
constexpr QSize infoSize(303, 32);
constexpr int infoTop = 42;

SpriteButton::Sprite collection(QRect normal, QRect hover = {}, QRect pressed = {})
{
    return {.file = "btn/collection.png", .normal = normal, .hover = hover, .pressed = pressed};
}

SpriteButton::Sprite likeSprite(int mark)
{
    const auto row = [](int index) { return QRect(0, index * 20, 22, 20); };
    switch (mark) {
    case 0:
        return {.file = "btn/like.png", .normal = row(0), .hover = row(1)};
    case 1:
        return {.file = "btn/like.png", .normal = row(2), .hover = row(3)};
    case 3:
        return {.file = "btn/like.png", .normal = row(5), .disabled = row(5)};
    default:
        return {.file = "btn/like.png", .normal = row(4), .disabled = row(4)};
    }
}

SpriteButton::Sprite favourSprite(bool favoured)
{
    if (favoured)
        return {.file = "btn/praised.png", .normal = {0, 0, 24, 24}};
    return {.file = "btn/praise.png", .hoverFile = "btn/praise_hvr.png", .normal = {0, 0, 24, 24}};
}

enum class Transport { Previous, Play, Pause, Next };

SpriteButton::Sprite transportSprite(const Skin &skin, Transport which)
{
    static constexpr QRect normal[]
        = {{0, 8, 20, 20}, {24, 0, 36, 36}, {64, 0, 36, 36}, {104, 8, 20, 20}};
    const auto image = normal[std::to_underlying(which)];
    if (const auto color = skin.buttonColor(); color.isValid())
        return {.file = "btn/color_btns.png", .normal = image, .tint = color};
    return collection(image, image.translated(0, 40));
}

}

class MiniPlayer::Cover final : public QAbstractButton
{
public:
    Cover(Skin &skin, QWidget *parent)
        : QAbstractButton(parent)
        , skin_(skin)
    {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(coverRect.size());
    }

    void setImage(const QPixmap &image)
    {
        const auto ratio = devicePixelRatioF();
        image_ = image.isNull()
                     ? QPixmap()
                     : image.scaled(size() * ratio, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        image_.setDevicePixelRatio(ratio);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.drawPixmap(rect(), image_.isNull() ? skin_.pixmap("mini/album/default.png") : image_);
        if (underMouse())
            painter.drawPixmap(rect(), skin_.pixmap("mini/album/highlight.png"));
    }

private:
    Skin &skin_;
    QPixmap image_;
};

class MiniPlayer::LyricsLine final : public QWidget
{
public:
    LyricsLine(Skin &skin, QWidget *parent)
        : QWidget(parent)
        , skin_(skin)
    {}

    void showLyric(const QString &line, double progress)
    {
        const int shift = qRound(overflow(line) * progress);
        if (line == lyric_ && shift == shift_)
            return;
        lyric_ = line;
        shift_ = shift;
        update();
    }

    void showTitle(const QString &title, const QString &artist)
    {
        if (lyric_.isEmpty() && title == title_ && artist == artist_)
            return;
        lyric_.clear();
        title_ = title;
        artist_ = artist;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        auto font = this->font();
        if (!lyric_.isEmpty()) {
            const auto box = lyricBox();
            const int overflow = this->overflow(lyric_);
            painter.setFont(lyricFont());
            painter.setPen(skin_.color(Skin::Color::MiniLyrics));
            painter.setClipRect(box);
            painter.drawText(
                box.adjusted(-shift_, 0, overflow, 0),
                (overflow ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignVCenter,
                lyric_);
            return;
        }
        const auto box = rect().adjusted(10, 7, -10, -5);
        const auto line =
            [&](int pixels, const QString &text, Skin::Color color, int top, int height) {
                font.setPixelSize(pixels);
                painter.setFont(font);
                painter.setPen(skin_.color(color));
                const QRect area(box.left(), top, box.width(), height);
                painter.drawText(
                    area,
                    Qt::AlignCenter,
                    QFontMetrics(font).elidedText(text, Qt::ElideRight, area.width()));
            };
        const int half = box.height() / 2;
        line(11, title_, Skin::Color::NoLyricsTitle, box.top(), half);
        line(10, artist_, Skin::Color::NoLyricsArtist, box.top() + half, box.height() - half);
    }

private:
    QFont lyricFont() const
    {
        auto font = this->font();
        font.setPixelSize(12);
        return font;
    }

    QRect lyricBox() const { return rect().adjusted(5, 0, -5, 0); }

    int overflow(const QString &line) const
    {
        return std::max(0, QFontMetrics(lyricFont()).horizontalAdvance(line) - lyricBox().width());
    }

    Skin &skin_;
    QString lyric_;
    int shift_ = 0;
    QString title_;
    QString artist_;
};

class MiniPlayer::InfoPanel final : public QWidget
{
public:
    explicit InfoPanel(Skin &skin)
        : QWidget(
              nullptr,
              Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
                  | Qt::WindowTransparentForInput)
        , skin_(skin)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFixedSize(infoSize);
    }

    void setText(const QString &title, const QString &artist)
    {
        title_ = title;
        artist_ = artist;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto left = skin_.pixmap("mini/bar/left.png");
        const auto right = skin_.pixmap("mini/bar/right.png");
        painter.drawPixmap(0, 0, left);
        painter.drawTiledPixmap(
            left.width(),
            0,
            width() - left.width() - right.width(),
            height(),
            skin_.pixmap("mini/bar/mid.png"));
        painter.drawPixmap(width() - right.width(), 0, right);

        const auto box = rect().adjusted(left.width() + 2, 4, -right.width() - 2, -4);
        const QFontMetrics metrics(font());
        const int gap = metrics.horizontalAdvance("  ");
        const auto title = metrics.elidedText(title_, Qt::ElideRight, box.width());
        const int titleWidth = metrics.horizontalAdvance(title);
        const auto artist
            = metrics
                  .elidedText(artist_, Qt::ElideRight, std::max(0, box.width() - titleWidth - gap));
        const int total = titleWidth
                          + (artist.isEmpty() ? 0 : gap + metrics.horizontalAdvance(artist));
        const int x = box.left() + (box.width() - total) / 2;
        painter.setPen(skin_.color(Skin::Color::InfoPanelTitle));
        painter.drawText(QRect(x, box.top(), titleWidth, box.height()), Qt::AlignVCenter, title);
        painter.setPen(skin_.color(Skin::Color::InfoPanelArtist));
        painter.drawText(
            QRect(x + titleWidth + gap, box.top(), box.right() - x, box.height()),
            Qt::AlignVCenter,
            artist);
    }

private:
    Skin &skin_;
    QString title_;
    QString artist_;
};

MiniPlayer::MiniPlayer(Skin &skin, QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , skin_(skin)
    , info_(std::make_unique<InfoPanel>(skin))
{
    setWindowTitle(tr("网易云音乐"));
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(barSize);

    buildBar();
    playlist_ = new MiniPlaylist(skin_, this);
    playlist_->setGeometry(playlistRect);
    playlist_->hide();

    setTrack({}, {}, TrackType::Normal);
    setLikeMark(2);
    applySkin();
    connect(&skin_, &Skin::changed, this, &MiniPlayer::applySkin);
}

MiniPlayer::~MiniPlayer() = default;

void MiniPlayer::buildBar()
{
    bar_ = new QFrame(this);
    bar_->setObjectName("top-panel-background");
    bar_->setGeometry(QRect({0, 0}, barSize));
    bar_->installEventFilter(this);
    auto *gradient = new QFrame(bar_);
    gradient->setObjectName("top-panel-gradient");
    gradient->setGeometry(gradientRect);
    gradient->setAttribute(Qt::WA_TransparentForMouseEvents);

    cover_ = new Cover(skin_, bar_);
    cover_->move(coverRect.topLeft());
    cover_->setToolTip(tr("完整模式"));
    connect(cover_, &QAbstractButton::clicked, this, [this] { emit fullModeRequested(true); });

    middle_ = new QStackedWidget(bar_);
    lyricsLine_ = new LyricsLine(skin_, middle_);
    middle_->addWidget(lyricsLine_);
    controls_ = new QWidget(middle_);
    middle_->addWidget(controls_);

    const auto button =
        [this](QWidget *parent, SpriteButton::Sprite sprite, const QString &tip, auto clicked) {
            auto *b = new SpriteButton(skin_, std::move(sprite), tip, parent);
            connect(b, &QAbstractButton::clicked, this, clicked);
            return b;
        };
    previous_ = button(controls_, transportSprite(skin_, Transport::Previous), tr("上一首"), [this] {
        emit action("prev");
    });
    playPause_ = button(controls_, transportSprite(skin_, Transport::Play), tr("播放"), [this] {
        emit action(playing_ ? "pause" : "play");
    });
    next_ = button(controls_, transportSprite(skin_, Transport::Next), tr("下一首"), [this] {
        emit action("next");
    });

    like_ = button(bar_, likeSprite(2), tr("喜欢"), [this] { emit likeClicked(); });
    favour_ = button(bar_, favourSprite(false), tr("赞"), [this] {
        emit favourClicked(favoured_ ? 1 : 0);
    });
    trash_ = button(
        bar_,
        {.file = "btn/delete.png", .hoverFile = "btn/delete_hvr.png", .normal = {0, 0, 21, 22}},
        tr("垃圾桶"),
        [this] { emit trashClicked(); });
    volume_ = button(
        bar_,
        {.file = "btn/volume.png", .normal = {0, 0, 22, 24}, .hover = {0, 24, 22, 24}},
        tr("音量"),
        [this] { emit action("volume"); });
    list_ = button(
        bar_,
        {.file = "btn/collection.png",
         .normal = {10, 100, 13, 16},
         .hover = {40, 100, 12, 16},
         .size = {22, 21},
         .offset = {5, 5}},
        tr("显示列表"),
        [this] { showPlaylist(!playlist_->isVisible()); });
    auto *close = button(bar_, collection({0, 80, 10, 10}, {}, {20, 80, 10, 10}), tr("关闭"), [this] {
        emit closeRequested();
    });
    fullMode_
        = button(bar_, collection({40, 80, 10, 10}, {}, {60, 80, 10, 10}), tr("完整模式"), [this] {
              emit fullModeRequested(false);
          });

    close->move(281, 7);
    fullMode_->move(281, 19);
    list_->move(253, 12);
    volume_->move(230, 12);
    trash_->move(206, 12);
}

void MiniPlayer::applySkin()
{
    setStyleSheet(skin_.styleSheet("mini-player.css"));
    previous_->setSprite(transportSprite(skin_, Transport::Previous));
    playPause_->setSprite(transportSprite(skin_, playing_ ? Transport::Pause : Transport::Play));
    next_->setSprite(transportSprite(skin_, Transport::Next));
    update();
}

void MiniPlayer::setTrack(const QString &title, const QString &artist, TrackType type)
{
    title_ = title;
    artist_ = artist;
    info_->setText(title, artist);

    trash_->setVisible(type == TrackType::Fm);
    like_->setVisible(type != TrackType::Radio);
    favour_->setVisible(type == TrackType::Radio);
    const int likeLeft = type == TrackType::Fm ? 181 : 204;
    like_->move(likeLeft, 13);
    favour_->move(204, 10);
    middle_->setGeometry(middleLeft, 0, likeLeft - 7 - middleLeft, barSize.height() - 2);

    int x = (middle_->width() - (20 + 4 + 36 + 4 + 20)) / 2;
    for (auto *b : {previous_, playPause_, next_}) {
        b->move(x, b->height() == 36 ? 5 : 13);
        x += b->width() + 4;
    }
    updateMiddle();
}

void MiniPlayer::setCover(const QPixmap &cover)
{
    cover_->setImage(cover);
}

void MiniPlayer::setPlaying(bool playing)
{
    playing_ = playing;
    playPause_->setSprite(transportSprite(skin_, playing ? Transport::Pause : Transport::Play));
    playPause_->setToolTip(playing ? tr("暂停") : tr("播放"));
    playlist_->setPlaying(playing);
}

void MiniPlayer::setLikeMark(int mark)
{
    like_->setSprite(likeSprite(mark));
    like_->setEnabled(mark == 0 || mark == 1);
    like_->setToolTip(mark == 1 || mark == 3 ? tr("取消喜欢") : tr("喜欢"));
}

void MiniPlayer::setFavour(bool favoured)
{
    favoured_ = favoured;
    favour_->setSprite(favourSprite(favoured));
    favour_->setToolTip(favoured ? tr("已赞") : tr("赞"));
}

void MiniPlayer::setLyrics(Lyrics lyrics)
{
    lyrics_ = std::move(lyrics);
    updateMiddle();
}

void MiniPlayer::setTime(qint64 milliseconds)
{
    time_ = milliseconds;
    if (isVisible() && !hovered_ && !lyrics_.isEmpty())
        updateMiddle();
}

void MiniPlayer::setFullModeTip(const QString &tip)
{
    fullMode_->setToolTip(tip);
    cover_->setToolTip(tip);
}

void MiniPlayer::setTopMost(bool topMost)
{
    setWindowFlagInPlace(*this, Qt::WindowStaysOnTopHint, topMost);
}

QRect MiniPlayer::volumeAnchor() const
{
    return {volume_->mapToGlobal(QPoint()), volume_->size()};
}

void MiniPlayer::updateMiddle()
{
    if (hovered_) {
        middle_->setCurrentWidget(controls_);
        return;
    }
    middle_->setCurrentWidget(lyricsLine_);
    const auto index = lyrics_.lineAt(time_);
    if (index < 0) {
        lyricsLine_->showTitle(title_, artist_);
        return;
    }
    const auto &line = lyrics_.line(index);
    const auto progress = line.duration > 0 ? double(time_ - line.start) / line.duration : 1.;
    lyricsLine_->showLyric(line.text, std::clamp(progress, 0., 1.));
}

void MiniPlayer::setHovered(bool hovered)
{
    hovered_ = hovered;
    updateMiddle();
    const bool info = hovered && !playlist_->isVisible() && !title_.isEmpty();
    if (info)
        info_->move(mapToGlobal(QPoint(0, infoTop)));
    info_->setVisible(info);
}

void MiniPlayer::showPlaylist(bool visible)
{
    setFixedSize(barSize.width(), visible ? playlistRect.bottom() + 1 : barSize.height());
    playlist_->setVisible(visible);
    list_->setToolTip(visible ? tr("隐藏列表") : tr("显示列表"));
    if (visible)
        info_->hide();
}

bool MiniPlayer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != bar_)
        return QWidget::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::Enter:
        setHovered(true);
        break;
    case QEvent::Leave:
        setHovered(false);
        break;
    case QEvent::MouseButtonPress:
        if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton && windowHandle())
            return windowHandle()->startSystemMove();
        break;
    case QEvent::ContextMenu:
        emit contextMenuRequested();
        return true;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void MiniPlayer::moveEvent(QMoveEvent *)
{
    if (info_->isVisible())
        info_->move(mapToGlobal(QPoint(0, infoTop)));
}

void MiniPlayer::hideEvent(QHideEvent *)
{
    info_->hide();
}

}
