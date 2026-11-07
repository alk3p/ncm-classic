#include "MiniPlaylist.h"

#include "Skin.h"
#include "bridge/JsonArgs.h"

#include <QContextMenuEvent>
#include <QJsonObject>
#include <QPainter>
#include <QStyledItemDelegate>

namespace ncm {

namespace {

enum Role { IdRole = Qt::UserRole, ArtistRole, ProgramRole };

constexpr int rowHeight = 28;
constexpr int markColumn = 20;
constexpr int infoColumn = 54;
constexpr QRect playingMark(70, 100, 10, 13);
constexpr QPoint playingMarkAt(4, 8);
constexpr QRect pausedMark(103, 160, 9, 11);
constexpr QPoint pausedMarkAt(5, 9);
constexpr QRect moreIcon(90, 95, 16, 17);
constexpr QRect moreIconHover(110, 95, 16, 17);

QRect moreButton(const QRect &row)
{
    return {row.right() - infoColumn + 1 + 16, row.top() + 6, moreIcon.width(), moreIcon.height()};
}

}

class MiniPlaylist::Delegate final : public QStyledItemDelegate
{
public:
    explicit Delegate(MiniPlaylist &list)
        : QStyledItemDelegate(&list)
        , list_(list)
    {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return {option.rect.width(), rowHeight};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index)
        const override
    {
        const auto &skin = list_.skin_;
        const auto &row = option.rect;
        const bool current = index.data(IdRole).toString() == list_.current_;
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;

        const auto background = selected  ? "mini/list/selected.png"
                                : hovered ? "mini/list/hover.png"
                                : current ? "mini/list/playing.png"
                                          : "mini/list/itmbk.png";
        painter->drawPixmap(row, skin.pixmap(background));
        if (!selected && !hovered && !current && index.row() % 2)
            painter->fillRect(row, QColor(127, 127, 127, 30));
        if (current) {
            const auto mark = list_.playing_ ? playingMark : pausedMark;
            const auto at = list_.playing_ ? playingMarkAt : pausedMarkAt;
            painter->drawPixmap(
                QRect(row.topLeft() + at, mark.size()), skin.pixmap("btn/collection.png"), mark);
        }

        const QRect text(
            row.left() + markColumn, row.top(), row.width() - markColumn - infoColumn, row.height());
        const QFontMetrics metrics(option.font);
        auto color = skin.color(
            selected ? Skin::Color::PlaylistItemSelected : Skin::Color::PlaylistItemText);
        const auto title = metrics.elidedText(index.data().toString(), Qt::ElideRight, text.width());
        painter->setFont(option.font);
        painter->setPen(color);
        painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter, title);
        const auto artist = index.data(ArtistRole).toString();
        if (const int used = metrics.horizontalAdvance(title);
            !artist.isEmpty() && used < text.width()) {
            color.setAlphaF(color.alphaF() * 0.6f);
            painter->setPen(color);
            painter->drawText(
                text.adjusted(used, 0, 0, 0),
                Qt::AlignLeft | Qt::AlignVCenter,
                metrics.elidedText(" - " + artist, Qt::ElideRight, text.width() - used));
        }

        if (index.data(ProgramRole).toBool()) {
            const auto badge = current || selected ? "mini/list/radio_hl.png"
                                                   : "mini/list/radio_nl.png";
            painter->drawPixmap(row.right() - infoColumn + 3, row.top() + 7, skin.pixmap(badge));
        } else if (hovered) {
            const auto button = moreButton(row);
            const bool pointed = button.contains(list_.viewport()->mapFromGlobal(QCursor::pos()));
            painter->drawPixmap(
                button, skin.pixmap("btn/collection.png"), pointed ? moreIconHover : moreIcon);
        }
    }

    bool editorEvent(
        QEvent *event,
        QAbstractItemModel *model,
        const QStyleOptionViewItem &option,
        const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease && !index.data(ProgramRole).toBool()) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton
                && moreButton(option.rect).contains(mouse->position().toPoint())) {
                emit list_.menuRequested(index.data(IdRole).toString());
                return true;
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    MiniPlaylist &list_;
};

MiniPlaylist::MiniPlaylist(Skin &skin, QWidget *parent)
    : QListWidget(parent)
    , skin_(skin)
{
    setObjectName("playlist-background");
    setItemDelegate(new Delegate(*this));
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setUniformItemSizes(true);
    setMouseTracking(true);
    connect(this, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        emit playRequested(item->data(IdRole).toString());
    });
    applySkin();
    connect(&skin_, &Skin::changed, this, &MiniPlaylist::applySkin);
}

void MiniPlaylist::applySkin()
{
    setStyleSheet(skin_.styleSheet("mini-player-playlist.css"));
    viewport()->update();
}

void MiniPlaylist::append(const QJsonArray &elements)
{
    for (const auto &value : elements) {
        const auto element = jsonObject(value);
        auto title = element["title"].toString();
        if (const auto alias = element["alias"].toString(); !alias.isEmpty())
            title += " (" + alias + ')';
        auto *item = new QListWidgetItem(title, this);
        item->setData(IdRole, element["id"].toVariant().toString());
        item->setData(ArtistRole, element["artist"].toString());
        item->setData(ProgramRole, !element["program"].isNull() && !element["program"].isUndefined());
    }
}

void MiniPlaylist::remove(const QStringList &ids)
{
    for (int row = count() - 1; row >= 0; --row)
        if (ids.contains(item(row)->data(IdRole).toString()))
            delete takeItem(row);
}

void MiniPlaylist::setCurrent(const QString &id)
{
    current_ = id;
    for (int row = 0; row < count(); ++row)
        if (item(row)->data(IdRole).toString() == id)
            scrollToItem(item(row));
    viewport()->update();
}

void MiniPlaylist::setPlaying(bool playing)
{
    playing_ = playing;
    viewport()->update();
}

QStringList MiniPlaylist::selectedIds() const
{
    QStringList ids;
    for (const auto *item : selectedItems())
        ids << item->data(IdRole).toString();
    return ids;
}

void MiniPlaylist::keyPressEvent(QKeyEvent *event)
{
    const auto key = event->keyCombination();
    if (key == QKeyCombination(Qt::CTRL, Qt::Key_F)) {
        emit shortcut(200, {});
        return;
    }
    int id = 0;
    if (key.key() == Qt::Key_Return || key.key() == Qt::Key_Enter)
        id = 40000;
    else if (key == QKeyCombination(Qt::CTRL, Qt::Key_S))
        id = 40004;
    else if (key == QKeyCombination(Qt::Key_L))
        id = 40006;
    else if (key.key() == Qt::Key_Delete)
        id = 40007;
    if (const auto tracks = selectedIds(); id && !tracks.isEmpty()) {
        emit shortcut(id, tracks);
        return;
    }
    QListWidget::keyPressEvent(event);
}

void MiniPlaylist::contextMenuEvent(QContextMenuEvent *event)
{
    if (auto *item = itemAt(event->pos())) {
        if (!item->isSelected())
            setCurrentItem(item);
        emit menuRequested(item->data(IdRole).toString());
    }
}

}
