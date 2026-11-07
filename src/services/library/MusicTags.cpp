#include "MusicTags.h"

#include "services/crypto/Crypto.h"

#include <memory>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <QMutex>

#include <taglib/fileref.h>
#include <taglib/mp4file.h>
#include <taglib/tag.h>
#include <taglib/tfilestream.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>

#ifdef Q_OS_WIN
#define TAGPATH(p) reinterpret_cast<const wchar_t *>((p).utf16())
#else
#define TAGPATH(p) QFile::encodeName(p).constData()
#endif

namespace ncm {

namespace {

std::shared_ptr<QMutex> fileMutex(const QString &path)
{
    static QMutex gate;
    static QHash<QString, std::weak_ptr<QMutex>> locks;
    QMutexLocker guard(&gate);
    if (locks.size() > 4096) {
        for (auto it = locks.begin(); it != locks.end();)
            if (it.value().expired())
                it = locks.erase(it);
            else
                ++it;
    }
    auto key = QFileInfo(path).canonicalFilePath();
    if (key.isEmpty())
        key = QFileInfo(path).absoluteFilePath();
    auto mutex = locks.value(key).lock();
    if (!mutex) {
        mutex = std::make_shared<QMutex>();
        locks[key] = mutex;
    }
    return mutex;
}

const QString markerPrefix = QStringLiteral("163 key(Don't modify):");
const QByteArray markerKey = "#14ljk_!\\]&0U<'(";

QString decodedComment(const QString &s)
{
    if (!s.startsWith(markerPrefix))
        return s;
    auto decoded = crypto::aesEcbDecrypt(
        QByteArray::fromBase64(s.mid(markerPrefix.size()).toLatin1()), markerKey);
    return decoded && !decoded->isEmpty() ? QString::fromUtf8(*decoded) : s;
}

std::optional<QString> encodedComment(const QString &s)
{
    if (!s.startsWith("music:") && !s.startsWith("dj:") && !s.startsWith("mv:"))
        return s;
    auto encrypted = crypto::aesEcbEncrypt(s.toUtf8(), markerKey);
    if (!encrypted)
        return std::nullopt;
    return markerPrefix + QString::fromLatin1(encrypted->toBase64());
}

QString qs(const TagLib::String &s)
{
    return QString::fromUtf8(s.toCString(true));
}

TagLib::String ts(const QString &s)
{
    return TagLib::String(s.toUtf8().constData(), TagLib::String::UTF8);
}

bool isMp4(const QString &path)
{
    TagLib::FileStream probe(TAGPATH(path), true);
    return TagLib::MP4::File::isSupported(&probe);
}

TagLib::FileRef openTags(
    const QString &path, TagLib::AudioProperties::ReadStyle style = TagLib::AudioProperties::Average)
{
    if (QFileInfo(path).suffix().compare("aac", Qt::CaseInsensitive) == 0 && isMp4(path))
        return TagLib::FileRef(new TagLib::MP4::File(TAGPATH(path), true, style));
    return TagLib::FileRef(TAGPATH(path), true, style);
}

}

bool MusicTags::supported(const QString &p)
{
    static const QStringList suffixes = {"mp3", "flac", "ogg",  "oga",  "opus", "m4a", "mp4",
                                         "aac", "wav",  "wave", "aiff", "aif",  "ape", "wv",
                                         "mpc", "wma",  "tta",  "spx",  "dsf",  "dff"};
    return suffixes.contains(QFileInfo(p).suffix().toLower());
}

std::optional<QJsonObject> MusicTags::read(const QString &p)
{
    auto lock = fileMutex(p);
    QMutexLocker guard(lock.get());
    const auto f = openTags(p, TagLib::AudioProperties::Accurate);
    if (f.isNull() || !f.audioProperties() || !f.tag())
        return std::nullopt;
    auto t = f.tag();
    auto a = f.audioProperties();
    QFileInfo fi(p);
    QJsonObject
        o{{"file", fi.absoluteFilePath()},
          {"title", qs(t->title())},
          {"album", qs(t->album())},
          {"artist", qs(t->artist())},
          {"genre", qs(t->genre())},
          {"comment", decodedComment(qs(t->comment()))},
          {"year", int(t->year())},
          {"index", int(t->track())},
          {"tracknumber", int(t->track())},
          {"duration", a->lengthInMilliseconds()},
          {"bitrate", a->bitrate()},
          {"samplerate", a->sampleRate()},
          {"channels", a->channels()},
          {"filesize", fi.size()},
          {"timestamp", fi.lastModified().toMSecsSinceEpoch()}};
    return o;
}

bool MusicTags::write(const QString &p, const QJsonObject &o, const QString &coverPath)
{
    auto lock = fileMutex(p);
    QMutexLocker guard(lock.get());
    auto f = openTags(p);
    if (f.isNull() || !f.tag() || f.file()->readOnly())
        return false;
    auto t = f.tag();
    if (o.contains("tit2"))
        t->setTitle(ts(o["tit2"].toString()));
    if (o.contains("tpe1"))
        t->setArtist(ts(o["tpe1"].toString()));
    if (o.contains("talb"))
        t->setAlbum(ts(o["talb"].toString()));
    if (o.contains("trck"))
        t->setTrack(o["trck"].toVariant().toUInt());
    if (o.contains("tpos")) {
        auto props = f.file()->properties();
        props["DISCNUMBER"] = TagLib::StringList(ts(o["tpos"].toString()));
        f.file()->setProperties(props);
    }
    if (o.contains("title"))
        t->setTitle(ts(o["title"].toString()));
    if (o.contains("name"))
        t->setTitle(ts(o["name"].toString()));
    if (o.contains("artist"))
        t->setArtist(ts(o["artist"].toString()));
    if (o.contains("album"))
        t->setAlbum(
            ts(o["album"].isObject() ? o["album"].toObject()["name"].toString()
                                     : o["album"].toString()));
    if (o.contains("genre"))
        t->setGenre(ts(o["genre"].toString()));
    if (o.contains("comment")) {
        auto comment = encodedComment(o["comment"].toString());
        if (!comment)
            return false;
        t->setComment(ts(*comment));
    }
    if (o.contains("year"))
        t->setYear(o["year"].toInt());
    if (o.contains("tracknumber"))
        t->setTrack(o["tracknumber"].toInt());
    if (o.contains("track"))
        t->setTrack(o["track"].toVariant().toUInt());
    if (o.contains("lyrics")) {
        auto props = f.file()->properties();
        props["LYRICS"] = TagLib::StringList(ts(o["lyrics"].toString()));
        f.file()->setProperties(props);
    }
    if (!coverPath.isEmpty()) {
        QFile image(coverPath);
        if (!image.open(QIODevice::ReadOnly))
            return false;
        auto data = image.readAll();
        if (image.error() != QFileDevice::NoError)
            return false;
        TagLib::VariantMap pic;
        pic["data"] = TagLib::ByteVector(data.constData(), data.size());
        pic["pictureType"] = TagLib::String("Front Cover");
        pic["mimeType"] = ts(QMimeDatabase().mimeTypeForData(data).name());
        TagLib::List<TagLib::VariantMap> list;
        list.append(pic);
        if (!f.file()->setComplexProperties("PICTURE", list))
            return false;
    }
    return f.save();
}

std::optional<MusicTags::Cover> MusicTags::cover(const QString &p)
{
    auto lock = fileMutex(p);
    QMutexLocker guard(lock.get());
    const auto f = openTags(p);
    if (f.isNull())
        return std::nullopt;
    auto pictures = f.file()->complexProperties("PICTURE");
    if (pictures.isEmpty())
        return std::nullopt;
    auto pic = pictures.front();
    auto b = pic["data"].toByteVector();
    if (b.isEmpty())
        return std::nullopt;
    return Cover{QByteArray(b.data(), b.size()), qs(pic["mimeType"].toString())};
}

std::optional<QString> MusicTags::lyrics(const QString &p)
{
    auto lock = fileMutex(p);
    QMutexLocker guard(lock.get());
    const auto f = openTags(p);
    if (f.isNull())
        return std::nullopt;
    auto props = f.file()->properties();
    if (!props.contains("LYRICS"))
        return std::nullopt;
    return qs(props["LYRICS"].toString());
}

}
