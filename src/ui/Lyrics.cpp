#include "Lyrics.h"

#include <algorithm>

#include <QFontMetrics>
#include <QRegularExpression>

namespace ncm {

namespace {

constexpr qint64 lastLineDuration = 5000;

QList<Lyrics::Line> parseKrc(const QString &krc)
{
    static const QRegularExpression lineTag(QStringLiteral(R"(^\[(\d+),(\d+)\](.*)$)"));
    static const QRegularExpression wordTag(
        QStringLiteral(R"([(<](\d+),(\d+)(?:,[^)>]*)?[)>]([^(<]*))"));
    QList<Lyrics::Line> lines;
    for (const auto &row : krc.split('\n')) {
        const auto match = lineTag.match(row.trimmed());
        if (!match.hasMatch())
            continue;
        Lyrics::Line line{match.captured(1).toLongLong(), match.captured(2).toLongLong(), {}, {}};
        for (const auto &word : wordTag.globalMatch(match.captured(3))) {
            line.words.append(
                {word.captured(1).toLongLong(), word.captured(2).toLongLong(), word.captured(3)});
            line.text += word.captured(3);
        }
        if (line.words.isEmpty())
            line.text = match.captured(3);
        if (!line.words.isEmpty() && line.words.front().start < line.start)
            for (auto &word : line.words)
                word.start += line.start;
        if (!line.text.trimmed().isEmpty())
            lines.append(std::move(line));
    }
    return lines;
}

QList<Lyrics::Line> parseLrc(const QString &lrc)
{
    static const QRegularExpression timeTag(QStringLiteral(R"(\[(\d+):(\d+(?:\.\d+)?)\])"));
    QList<Lyrics::Line> lines;
    for (const auto &row : lrc.split('\n')) {
        auto text = row;
        text.remove(timeTag);
        text = text.trimmed();
        for (const auto &tag : timeTag.globalMatch(row)) {
            const auto seconds = tag.captured(1).toDouble() * 60 + tag.captured(2).toDouble();
            lines.append({qint64(seconds * 1000), 0, text, {}});
        }
    }
    std::ranges::stable_sort(lines, {}, &Lyrics::Line::start);
    for (qsizetype i = 0; i < lines.size(); ++i)
        lines[i].duration = i + 1 < lines.size() ? lines[i + 1].start - lines[i].start
                                                 : lastLineDuration;
    lines.removeIf([](const Lyrics::Line &line) { return line.text.isEmpty(); });
    return lines;
}

}

Lyrics Lyrics::parse(const QString &krc, const QString &lrc)
{
    Lyrics lyrics;
    lyrics.lines_ = parseKrc(krc);
    if (lyrics.lines_.isEmpty())
        lyrics.lines_ = parseLrc(lrc);
    return lyrics;
}

qsizetype Lyrics::lineAt(qint64 time) const
{
    const auto next = std::ranges::upper_bound(lines_, time, {}, &Line::start);
    return std::distance(lines_.begin(), next) - 1;
}

double Lyrics::progress(qsizetype index, qint64 time, const QFontMetrics &metrics) const
{
    const auto &line = lines_[index];
    if (line.words.isEmpty()) {
        if (line.duration <= 0)
            return 1;
        return std::clamp(double(time - line.start) / double(line.duration), 0., 1.);
    }
    double sung = 0;
    for (const auto &word : line.words) {
        const auto width = metrics.horizontalAdvance(word.text);
        if (time >= word.start + word.duration) {
            sung += width;
            continue;
        }
        if (time > word.start && word.duration > 0)
            sung += width * double(time - word.start) / word.duration;
        break;
    }
    return std::clamp(sung / std::max(1, metrics.horizontalAdvance(line.text)), 0., 1.);
}

}
