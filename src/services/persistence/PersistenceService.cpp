#include "PersistenceService.h"

#include "bridge/Bridge.h"
#include "bridge/JsonArgs.h"
#include "services/library/MusicTags.h"

#include <algorithm>
#include <optional>
#include <utility>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryFile>

namespace ncm {

namespace {

bool save(const QString &path, const QByteArray &data)
{
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
}

QString tempPath(const Profile &p, const QString &key)
{
    return p.paths.tempFiles + "/"
           + QString::fromLatin1(
               QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

QStringList strings(const QJsonValue &v)
{
    QStringList r;
    if (v.isArray())
        for (const auto x : v.toArray())
            r << x.toString();
    else if (v.isString())
        r << v.toString();
    return r;
}

std::optional<QSet<QString>> completedDownloads(SqlDatabase &db, const QString &base)
{
    const auto r = db.execute("SELECT relative_path FROM web_offline_track WHERE state=4");
    if (!r)
        return std::nullopt;
    QSet<QString> files;
    for (const auto row : r.rows) {
        auto path = row.toObject().value("relative_path").toString();
        if (!QDir::isAbsolutePath(path))
            path = QDir(base).filePath(path);
        if (const auto canonical = QFileInfo(path).canonicalFilePath(); !canonical.isEmpty())
            files.insert(canonical);
    }
    return files;
}

struct Executed
{
    SqlResult result;
    std::optional<QSet<QString>> downloads;
};

}

PersistenceService::PersistenceService(Bridge &b, const RuntimePaths &paths, QObject *parent)
    : QObject(parent)
    , bridge_(b)
    , profile_(paths)
    , settings_(profile_)
    , online_(paths.dataRoot + "/OnlineLibrary.db", ":/ncm/assets/misc/pinyin.dict")
    , library_(b, profile_, this)
{
    if (const auto schema = online_.initialize(false); !schema)
        qWarning() << "Online schema:" << schema.message;
    pool_.setMaxThreadCount(2);
    refreshDownloads();
    registerHandlers();
}

PersistenceService::~PersistenceService()
{
    stopping_.store(true);
    pool_.clear();
    pool_.waitForDone();
}

bool PersistenceService::containsMusicFile(const QString &path) const
{
    const auto canonical = QFileInfo(path).canonicalFilePath();
    return !canonical.isEmpty()
           && (library_.containsFile(canonical) || knownDownloads_.contains(canonical));
}

void PersistenceService::refreshDownloads()
{
    online_.run(
        this,
        [base = downloadPath()](SqlDatabase &db) { return completedDownloads(db, base); },
        [this](std::optional<QSet<QString>> files) {
            if (files)
                knownDownloads_ = std::move(*files);
        });
}

QString PersistenceService::downloadPath() const
{
    return settings_.get("ServiceStorage", "download-path", profile_.paths.downloadRoot).toString();
}

QString PersistenceService::cachePath() const
{
    return settings_.get("ServiceStorage", "cache-path", profile_.paths.storageCache).toString();
}

QString PersistenceService::resolveFile(const QString &type, const QString &path) const
{
    if (type == "abs" || QDir::isAbsolutePath(path))
        return QDir::cleanPath(path);
    if (path.startsWith("webdata/") || path.startsWith("webdata\\"))
        return profile_.relativeFile(path);
    const auto clean = QDir::cleanPath(path);
    if (clean == ".." || clean.startsWith("../"))
        return {};
    return QDir(downloadPath()).filePath(clean);
}

QJsonArray PersistenceService::cacheTracks(const QJsonObject &match) const
{
    QJsonArray out;
    QDirIterator it(cachePath(), {"*.cache.json"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile f(it.next());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const auto o = QJsonDocument::fromJson(f.readAll()).object();
        const auto path = o["path"].toString();
        const QFileInfo fi(path);
        if (!fi.isFile() || path.endsWith(".part") || o["cached"].toInt() != 100)
            continue;
        if (o.contains("size") && o["size"].toInteger() != fi.size())
            continue;
        const bool accept = std::ranges::all_of(match.keys(), [&](const QString &key) {
            return match[key].toVariant().toString() == o[key].toVariant().toString();
        });
        if (accept)
            out.append(o);
    }
    return out;
}

void PersistenceService::registerHandlers()
{
    for (const auto method : {"execsql", "exectransaction"})
        bridge_.addHandler(
            "storage." + QString(method),
            [this,
             atomic = qstrcmp(method, "exectransaction")
                      == 0](const QJsonArray &a, Bridge::Reply reply) {
                const auto sql = strings(a.at(1));
                const bool downloads
                    = sql.join('\n').contains("web_offline_track", Qt::CaseInsensitive);
                online_.run(
                    this,
                    [sql, atomic, downloads, base = downloadPath()](SqlDatabase &db) {
                        Executed done{db.execute(sql, atomic), std::nullopt};
                        if (downloads && done.result.modified)
                            done.downloads = completedDownloads(db, base);
                        return done;
                    },
                    [this, id = a.at(0), reply](Executed done) {
                        if (done.downloads)
                            knownDownloads_ = std::move(*done.downloads);
                        const QJsonArray out{id, done.result.error, done.result.rows};
                        bridge_.publish("storage.onexecsqldone", out);
                        reply(out);
                    });
            });
    bridge_.addHandler("storage.init", [this](const QJsonArray &a, Bridge::Reply reply) {
        auto download = strings(a.at(0)).value(0);
        if (download.isEmpty())
            download = downloadPath();
        auto cache = stringArg(a, 2);
        if (cache.isEmpty())
            cache = cachePath();
        const double capacity
            = numberArg(a, 1, settings_.get("ServiceStorage", "cache-capacity", 1).toDouble());
        const bool ok = capacity >= 0 && QDir().mkpath(download) && QDir().mkpath(cache)
                        && settings_.set("ServiceStorage", "download-path", download)
                        && settings_.set("ServiceStorage", "cache-path", cache)
                        && settings_.set("ServiceStorage", "cache-capacity", capacity);
        reply({ok ? download : QString(), ok ? cache : QString()});
    });
    bridge_.addHandler("storage.readfromfile", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto path = profile_.relativeFile(stringArg(a, 1));
        QFile f(path);
        const bool ok = !path.isEmpty() && f.open(QIODevice::ReadOnly);
        const QJsonArray out{a.at(0), ok ? 0 : 1, ok ? QString::fromUtf8(f.readAll()) : QString()};
        bridge_.publish("storage.onreadfromfiledone", out);
        reply(out);
    });
    bridge_.addHandler("storage.savetofile", [this](const QJsonArray &a, Bridge::Reply reply) {
        const bool ok = save(profile_.relativeFile(stringArg(a, 3)), stringArg(a, 1).toUtf8());
        const QJsonArray out{a.at(0), ok ? 0 : 1};
        bridge_.publish("storage.onsavetofiledone", out);
        reply(out);
    });
    bridge_.addHandler("storage.listFile", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto path = resolveFile(stringArg(a, 1), stringArg(a, 3));
        const QDir dir(path);
        const bool ok = !path.isEmpty() && dir.exists();
        QJsonArray files;
        if (ok)
            for (const auto &f :
                 dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
                files.append(
                    QJsonObject{
                        {"name", f.fileName()},
                        {"path", f.absoluteFilePath()},
                        {"size", f.size()},
                        {"isDir", f.isDir()},
                        {"type", f.isDir() ? "directory" : "file"}});
        const QJsonArray out{a.at(0), ok ? 0 : 1, files};
        bridge_.publish("storage.onlistfile", out);
        reply(out);
    });
    bridge_.addHandler("storage.deletefile", [this](const QJsonArray &a, Bridge::Reply reply) {
        QJsonArray deleted, failed;
        for (const auto &raw : strings(a.at(3))) {
            const auto path = resolveFile(stringArg(a, 1), raw);
            const QFileInfo fi(path);
            if (path.isEmpty()
                || (fi.exists() && !(fi.isFile() && QFile::remove(path))
                    && !(fi.isDir() && QDir().rmdir(path)))) {
                failed.append(raw);
                continue;
            }
            deleted.append(raw);
            if (boolArg(a, 4)) {
                const QDir folder(fi.absolutePath());
                if (folder.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot))
                    QDir().rmdir(folder.absolutePath());
            }
        }
        const QJsonArray out{a.at(0), failed.isEmpty() ? 0 : 1, deleted};
        bridge_.publish("storage.ondeletefilesdone", out);
        reply(out);
    });
    for (const auto name : {"copyfiles", "movefiles"})
        bridge_.addHandler(
            "storage." + QString(name),
            [this,
             move = qstrcmp(name, "movefiles") == 0](const QJsonArray &a, Bridge::Reply reply) {
                const auto sources = strings(a.at(3)), destinations = strings(a.at(6));
                QStringList from, to;
                for (qsizetype i = 0; i < sources.size(); ++i) {
                    from << resolveFile(stringArg(a, 1), sources[i]);
                    to << resolveFile(stringArg(a, 4), destinations.value(i));
                }
                pool_.start([this, sources, from, to, move, id = a.at(0), reply] {
                    QJsonArray errors;
                    for (qsizetype i = 0; i < sources.size(); ++i) {
                        bool ok = !stopping_.load() && !from[i].isEmpty() && !to[i].isEmpty()
                                  && from[i] != to[i] && !QFile::exists(to[i])
                                  && QDir().mkpath(QFileInfo(to[i]).absolutePath());
                        if (ok) {
                            ok = move ? QFile::rename(from[i], to[i]) : QFile::copy(from[i], to[i]);
                            if (!ok && move && !QFile::exists(to[i])) {
                                ok = QFile::copy(from[i], to[i]);
                                if (ok && !QFile::remove(from[i])) {
                                    QFile::remove(to[i]);
                                    ok = false;
                                }
                            }
                        }
                        if (!ok)
                            errors.append(sources[i]);
                        const QJsonObject progress{
                            {"code", ok ? 0 : 1},
                            {"state", i + 1 == sources.size() ? 0 : 1},
                            {"errcode", ok ? 0 : 1},
                            {"remain", sources.size() - i - 1},
                            {"process", double(i + 1) / sources.size()},
                            {"src", from[i]},
                            {"dst", to[i]}};
                        QMetaObject::invokeMethod(this, [this, move, progress] {
                            bridge_.publish(
                                move ? "storage.onmoveprocess" : "storage.oncopyprocess",
                                {progress});
                        });
                    }
                    QMetaObject::invokeMethod(this, [id, errors, reply] {
                        reply({id, errors.isEmpty() ? 0 : 1, errors});
                    });
                });
            });
    bridge_.addHandler("storage.testwriteable", [](const QJsonArray &a, Bridge::Reply reply) {
        QTemporaryFile f(QDir(stringArg(a, 0)).filePath(".ncm-write-test-XXXXXX"));
        reply({f.open()});
    });
    bridge_.addHandler("storage.getTempFile", [this](const QJsonArray &a, Bridge::Reply reply) {
        QFile f(tempPath(profile_, stringArg(a, 0)));
        const bool ok = f.open(QIODevice::ReadOnly);
        const QJsonArray out{a.at(0), ok ? 0 : 1, ok ? QString::fromUtf8(f.readAll()) : QString()};
        bridge_.publish("storage.ongettempfile", out);
        reply(out);
    });
    bridge_.addHandler("storage.updatetemp", [this](const QJsonArray &a, Bridge::Reply reply) {
        const bool ok = save(tempPath(profile_, stringArg(a, 0)), stringArg(a, 1).toUtf8());
        bridge_.publish("storage.onupdatetemp", {a.at(0), ok});
        reply({ok});
    });
    bridge_.addHandler("storage.checkFilesExist", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto base = stringArg(a, 2).isEmpty() ? downloadPath() : stringArg(a, 2);
        QJsonArray result;
        for (const auto item : a.at(1).toArray()) {
            auto o = item.toObject();
            auto path = o["path"].toString();
            if (!QDir::isAbsolutePath(path))
                path = QDir(base).filePath(path);
            const QFileInfo fi(path);
            o["exist"] = fi.exists();
            o["size"] = fi.size();
            result.append(o);
        }
        const QJsonArray out{a.at(0), true, result};
        bridge_.publish("storage.oncheckfilesexist", out);
        reply(out);
    });
    bridge_.addHandler("storage.imagesInfo", [this](const QJsonArray &a, Bridge::Reply reply) {
        QJsonArray images;
        for (const auto &path : strings(a.at(1))) {
            QImageReader reader(path);
            const auto size = reader.size();
            images.append(
                QJsonObject{
                    {"path", path},
                    {"code", size.isValid() ? 0 : 1},
                    {"width", size.width()},
                    {"height", size.height()},
                    {"size", QFileInfo(path).size()},
                    {"format", QString::fromLatin1(reader.format())}});
        }
        bridge_.publish("storage.onImagesInfo", {a.at(0), images});
        reply({a.at(0), images});
    });
    bridge_.addHandler("storage.scaleImage", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto options = a.at(1).toObject();
        const auto path = options["path"].toString();
        QImageReader reader(path);
        reader.setAutoTransform(true);
        auto image = reader.read();
        const int width = qBound(1, intField(options, "width", 2048), 16384);
        const int height = qBound(1, intField(options, "height", 2048), 16384);
        switch (intField(options, "scale_type")) {
        case 1:
            image = image.scaledToHeight(height, Qt::SmoothTransformation);
            break;
        case 2:
            image = image.scaledToWidth(width, Qt::SmoothTransformation);
            break;
        default:
            image = image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        const auto scaled = tempPath(profile_, path) + ".jpg";
        const bool ok = !image.isNull() && image.save(scaled, "JPG", 80);
        const QJsonArray out{a.at(0), ok ? 0 : 1, ok ? scaled : QString()};
        bridge_.publish("storage.onScaleImage", out);
        reply(out);
    });
    bridge_.addHandler("storage.queryCacheTracks", [this](const QJsonArray &, Bridge::Reply reply) {
        const auto tracks = cacheTracks();
        bridge_.publish("storage.onquerycachetracks", {tracks});
        reply({tracks});
    });
    bridge_
        .addHandler("storage.queryNewCacheTracks", [this](const QJsonArray &a, Bridge::Reply reply) {
            const auto tracks = cacheTracks(a.at(1).toObject());
            bridge_.publish("storage.onquerynewcachetracks", {a.at(0), tracks});
            reply({a.at(0), tracks});
        });
    bridge_.addHandler("storage.clearCache", [this](const QJsonArray &a, Bridge::Reply reply) {
        auto folders = strings(a.at(0));
        if (folders.isEmpty())
            folders << cachePath();
        bool ok = true;
        for (auto folder : folders) {
            if (folder.isEmpty())
                folder = cachePath();
            const auto base = QDir(folder).absolutePath() + "/";
            QDirIterator it(folder, {"*.cache.json"}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const auto metadata = it.next();
                QFile f(metadata);
                if (!f.open(QIODevice::ReadOnly)) {
                    ok = false;
                    continue;
                }
                const auto file = QJsonDocument::fromJson(f.readAll()).object()["path"].toString();
                f.close();
                const auto absolute = QFileInfo(file).absoluteFilePath();
                if (bridge_.property("ncm.activeMediaPaths").toStringList().contains(absolute))
                    continue;
                if (!absolute.startsWith(base) || absolute.endsWith(".part")
                    || (QFile::exists(file) && !QFile::remove(file))) {
                    ok = false;
                    continue;
                }
                ok = QFile::remove(metadata) && ok;
            }
        }
        bridge_.publish("storage.onclearcache", {ok});
        reply({ok});
    });
    bridge_.addHandler("storage.addid3", [this](const QJsonArray &a, Bridge::Reply reply) {
        auto info = a.at(4).toObject();
        info["comment"] = stringArg(a, 3);
        pool_.start(
            [this, id = a.at(0), path = stringArg(a, 1), cover = stringArg(a, 2), info, reply] {
                const bool ok = MusicTags::write(path, info, cover);
                QMetaObject::invokeMethod(this, [this, id, ok, reply] {
                    bridge_.publish("storage.onaddid3done", {id, ok ? 1 : 0});
                    reply({id, ok ? 1 : 0});
                });
            });
    });
    bridge_.addHandler("storage.downloadscanner", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto root = stringArg(a, 0).isEmpty() ? downloadPath() : stringArg(a, 0);
        const int limit = qBound(1, int(numberArg(a, 3, 100)), 1000);
        pool_.start([this, root, limit, known = strings(a.at(4)), reply] {
            QJsonArray batch;
            QDirIterator it(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (it.hasNext() && !stopping_.load()) {
                const auto path = it.next();
                const auto relative = QDir(root).relativeFilePath(path);
                if (known.contains(relative) || !MusicTags::supported(path))
                    continue;
                const auto info = MusicTags::read(path);
                if (!info)
                    continue;
                const QFileInfo fi(path);
                batch.append(
                    QJsonObject{
                        {"path", relative},
                        {"comment", info->value("comment")},
                        {"size", fi.size()},
                        {"creation_time", fi.birthTime().toMSecsSinceEpoch()}});
                if (batch.size() >= limit)
                    QMetaObject::invokeMethod(this, [this, found = std::exchange(batch, {})] {
                        bridge_.publish("storage.ondownloadscanner", {found});
                    });
            }
            QMetaObject::invokeMethod(this, [this, batch, reply] {
                if (!batch.isEmpty())
                    bridge_.publish("storage.ondownloadscanner", {batch});
                reply({});
            });
        });
    });
}

}
