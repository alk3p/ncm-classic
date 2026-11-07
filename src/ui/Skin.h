#pragma once

#include <array>

#include <QColor>
#include <QObject>
#include <QPixmap>

namespace ncm {

class Skin final : public QObject
{
    Q_OBJECT

public:
    enum class Color {
        MenuItemText,
        NoLyricsTitle,
        NoLyricsArtist,
        InfoPanelTitle,
        InfoPanelArtist,
        PlaylistItemText,
        PlaylistItemSelected,
        MiniLyrics,
        LyricsOutline,
        Count,
    };

    explicit Skin(QObject *parent = nullptr);

    bool load(const QString &name, const QColor &buttonColor = {});

    QString file(const QString &relative) const;
    QPixmap pixmap(const QString &relative) const;
    QColor color(Color) const;
    QColor buttonColor() const { return buttonColor_; }
    QString styleSheet(const QString &name) const;

signals:
    void changed();

private:
    void readColors();

    QString name_ = "default";
    QColor buttonColor_;
    std::array<QColor, std::size_t(Color::Count)> colors_;
};

}
