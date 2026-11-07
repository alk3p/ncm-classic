#include "CueParser.h"

#include "MusicTags.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTextCodec>

namespace ncm {

std::expected<QJsonObject, QString> CueParser::parse(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::unexpected(f.errorString());
    auto bytes = f.readAll();
    if (f.error() != QFileDevice::NoError)
        return std::unexpected(f.errorString());
    QString text;
    auto encoding = QStringConverter::encodingForData(bytes);
    if (encoding) {
        text = QStringDecoder(*encoding)(bytes);
    } else {
        QStringDecoder utf8(QStringDecoder::Utf8);
        text = utf8(bytes);
        if (utf8.hasError()) {
            auto codec = QTextCodec::codecForName("GB18030");
            text = codec ? codec->toUnicode(bytes) : QString::fromLocal8Bit(bytes);
        }
    }
    QString album, performer, genre, date, audio;
    QJsonArray tracks;
    QJsonObject current;
    auto finishTrack = [&]() -> std::expected<void, QString> {
        if (current.isEmpty())
            return {};
        if (!current.contains("start"))
            return std::unexpected(QStringLiteral("Track has no INDEX 01"));
        tracks.append(current);
        current = {};
        return {};
    };
    QRegularExpression linePattern("^\\s*(\\w+)\\s+(.*)$"),
        filePattern("^\"([^\"]+)\"(?:\\s+\\w+)?$"), timePattern("^(\\d+)\\s+(\\d+):(\\d+):(\\d+)$");
    auto unquote = [](QString s) {
        s = s.trimmed();
        if (s.startsWith('"') && s.endsWith('"'))
            s = s.mid(1, s.size() - 2);
        return s;
    };
    for (const auto &line : text.split(QRegularExpression("[\\r\\n]+"))) {
        auto m = linePattern.match(line);
        if (!m.hasMatch())
            continue;
        auto key = m.captured(1).toUpper(), value = m.captured(2).trimmed();
        if (key == "FILE") {
            auto fm = filePattern.match(value);
            auto name = fm.hasMatch() ? fm.captured(1) : value.section(' ', 0, -2);
            audio
                = QFileInfo(QDir(QFileInfo(path).absolutePath()).filePath(name)).absoluteFilePath();
        } else if (key == "TRACK") {
            if (auto track = finishTrack(); !track)
                return std::unexpected(track.error());
            if (value.section(' ', 1).trimmed().compare("AUDIO", Qt::CaseInsensitive) == 0)
                current = {{"index", value.section(' ', 0, 0).toInt()}, {"type", 100}};
        } else if (key == "TITLE") {
            if (current.isEmpty())
                album = unquote(value);
            else
                current["title"] = unquote(value);
        } else if (key == "PERFORMER") {
            if (current.isEmpty())
                performer = unquote(value);
            else
                current["performer"] = unquote(value);
        } else if (key == "INDEX" && !current.isEmpty()) {
            auto tm = timePattern.match(value);
            if (tm.hasMatch() && tm.captured(1).toInt() == 1) {
                int sec = tm.captured(3).toInt(), frames = tm.captured(4).toInt();
                if (sec >= 60 || frames >= 75)
                    return std::unexpected(QStringLiteral("Invalid CUE time"));
                current["audiopath"] = audio;
                current["start"] = (tm.captured(2).toInt() * 60.0 + sec + frames / 75.0) * 1000.0;
            }
        } else if (key == "REM") {
            auto field = value.section(' ', 0, 0).toUpper();
            if (field == "GENRE")
                genre = unquote(value.section(' ', 1));
            if (field == "DATE")
                date = unquote(value.section(' ', 1));
        }
    }
    if (auto track = finishTrack(); !track)
        return std::unexpected(track.error());
    if (tracks.isEmpty())
        return std::unexpected(QStringLiteral("CUE contains no audio tracks"));
    QHash<QString, QJsonObject> metadata;
    QHash<qint64, QString> firstAudio;
    const auto cuePath = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < tracks.size(); ++i) {
        auto t = tracks[i].toObject();
        auto p = t["audiopath"].toString();
        if (!metadata.contains(p)) {
            auto info = MusicTags::read(p);
            if (!info)
                return std::unexpected("Unsupported or unreadable audio: " + p);
            metadata[p] = *info;
        }
        auto info = metadata[p];
        double start = t["start"].toDouble(), end = info["duration"].toDouble();
        if (i + 1 < tracks.size() && tracks[i + 1].toObject()["audiopath"].toString() == p)
            end = tracks[i + 1].toObject()["start"].toDouble();
        if (end < start)
            return std::unexpected(
                QStringLiteral("CUE indices exceed audio duration or are out of order"));
        t["duration"] = end - start;
        t["filesize"] = qint64(
            info["filesize"].toInteger() * (end - start) / qMax(1.0, info["duration"].toDouble()));
        t["bitrate"] = info["bitrate"];
        t["album"] = album;
        t["artist"] = t["performer"].toString(performer);
        t["genre"] = genre;
        t["year"] = date.toInt();
        const auto offset = qint64(start);
        if (auto first = firstAudio.constFind(offset); first == firstAudio.cend())
            firstAudio.insert(offset, p);
        else if (*first != p) {
            auto key = cuePath + "cueAudio=" + p + "cueOffset=" + QString::number(offset);
            t["id"] = QString::fromLatin1(
                QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
        }
        tracks[i] = t;
    }
    return QJsonObject{
        {"cuepath", cuePath},
        {"audiopath", tracks.first().toObject()["audiopath"]},
        {"title", album},
        {"performer", performer},
        {"genre", genre},
        {"date", date},
        {"bitrate", tracks.first().toObject()["bitrate"]},
        {"tracks", tracks}};
}

}
