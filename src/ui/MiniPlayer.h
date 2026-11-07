#pragma once

#include "Lyrics.h"

#include <memory>

#include <QPixmap>
#include <QWidget>

class QFrame;
class QStackedWidget;

namespace ncm {

class MiniPlaylist;
class Skin;
class SpriteButton;

class MiniPlayer final : public QWidget
{
    Q_OBJECT

public:
    enum class TrackType { Normal, Fm, Radio };

    explicit MiniPlayer(Skin &, QWidget *parent = nullptr);
    ~MiniPlayer() override;

    void setTrack(const QString &title, const QString &artist, TrackType);
    void setCover(const QPixmap &);
    void setPlaying(bool);
    void setLikeMark(int);
    void setFavour(bool);
    void setLyrics(Lyrics);
    void setTime(qint64 milliseconds);
    void setFullModeTip(const QString &);
    void setTopMost(bool);

    MiniPlaylist &playlist() { return *playlist_; }
    QRect volumeAnchor() const;

signals:
    void action(const QString &name);
    void fullModeRequested(bool detail);
    void closeRequested();
    void contextMenuRequested();
    void likeClicked();
    void favourClicked(int mark);
    void trashClicked();

protected:
    bool eventFilter(QObject *, QEvent *) override;
    void moveEvent(QMoveEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    class Cover;
    class LyricsLine;
    class InfoPanel;

    void buildBar();
    void applySkin();
    void setHovered(bool);
    void showPlaylist(bool);
    void updateMiddle();

    Skin &skin_;
    QString title_;
    QString artist_;
    Lyrics lyrics_;
    qint64 time_ = 0;
    bool playing_ = false;
    bool favoured_ = false;
    bool hovered_ = false;

    QFrame *bar_;
    Cover *cover_;
    QStackedWidget *middle_;
    LyricsLine *lyricsLine_;
    QWidget *controls_;
    SpriteButton *previous_;
    SpriteButton *playPause_;
    SpriteButton *next_;
    SpriteButton *like_;
    SpriteButton *favour_;
    SpriteButton *trash_;
    SpriteButton *volume_;
    SpriteButton *list_;
    SpriteButton *fullMode_;
    MiniPlaylist *playlist_;
    std::unique_ptr<InfoPanel> info_;
};

}
