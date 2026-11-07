#pragma once

#include <QList>
#include <QString>

class QFontMetrics;

namespace ncm {

class Lyrics
{
public:
    struct Word
    {
        qint64 start;
        qint64 duration;
        QString text;
    };

    struct Line
    {
        qint64 start;
        qint64 duration;
        QString text;
        QList<Word> words;
    };

    static Lyrics parse(const QString &krc, const QString &lrc);

    bool isEmpty() const { return lines_.isEmpty(); }
    qsizetype size() const { return lines_.size(); }
    const Line &line(qsizetype index) const { return lines_[index]; }
    qsizetype lineAt(qint64 time) const;
    double progress(qsizetype index, qint64 time, const QFontMetrics &) const;

private:
    QList<Line> lines_;
};

}
