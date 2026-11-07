#pragma once

#include <QJsonArray>
#include <QListWidget>

namespace ncm {

class Skin;

class MiniPlaylist final : public QListWidget
{
    Q_OBJECT

public:
    explicit MiniPlaylist(Skin &, QWidget *parent = nullptr);

    void append(const QJsonArray &elements);
    void remove(const QStringList &ids);
    void setCurrent(const QString &id);
    void setPlaying(bool);

signals:
    void playRequested(const QString &id);
    void menuRequested(const QString &id);
    void shortcut(int id, const QStringList &tracks);

protected:
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

private:
    class Delegate;

    void applySkin();
    QStringList selectedIds() const;

    Skin &skin_;
    QString current_;
    bool playing_ = false;
};

}
