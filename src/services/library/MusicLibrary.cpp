#include "MusicLibrary.h"

#include "CueParser.h"
#include "MusicTags.h"
#include "bridge/Bridge.h"

#include <expected>
#include <utility>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSemaphore>
#include <QStandardPaths>

namespace ncm {

namespace {

constexpr auto missingLibrary = "Library path does not exist";
constexpr double minimumDurationMs = 30000;

const QStringList scannedColumns{
    "file",
    "id",
    "dir",
    "title",
    "album",
    "genre",
    "artist",
    "duration",
    "timestamp",
    "bitrate",
    "filesize",
    "parentdir",
    "librarypath",
    "tracknumber",
    "source",
    "starttime",
    "type"};

QString hash(const QString &s)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Md5).toHex());
}

QString sqlValue(const QJsonValue &v)
{
    if (v.isNull() || v.isUndefined())
        return "NULL";
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 17);
    return SqlDatabase::quote(v.toVariant().toString());
}

bool isCue(const QFileInfo &file)
{
    return file.suffix().compare("cue", Qt::CaseInsensitive) == 0;
}

qint64 modified(const QFileInfo &file)
{
    return file.lastModified().toMSecsSinceEpoch();
}

struct Accepted
{
    SqlResult result;
    int added = 0;
    QStringList files;
};

}

MusicLibrary::MusicLibrary(Bridge &bridge, const Profile &profile, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , db_(profile.paths.dataRoot + "/OfflineLibrary.db", ":/ncm/assets/misc/pinyin.dict")
{
    if (const auto schema = db_.initialize(true); !schema)
        qWarning() << "Offline schema:" << schema.message;
    pool_.setMaxThreadCount(2);
    refreshKnown();
    registerHandlers();

    debounce_.setSingleShot(true);
    debounce_.setInterval(350);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) {
        for (const auto &key : std::as_const(observed_))
            if (path.startsWith(resolveLibrary(key)))
                dirty_.insert(key);
        debounce_.start();
    });
    connect(&debounce_, &QTimer::timeout, this, [this] {
        for (const auto &key : std::exchange(dirty_, {})) {
            if (!scans_.contains(key))
                scan(key);
            bridge_.publish("musiclibrary.onobserveLibrary", {key});
        }
    });
}

MusicLibrary::~MusicLibrary()
{
    for (const auto &running : std::as_const(scans_))
        running->store(true);
    pool_.clear();
    pool_.waitForDone();
}

void MusicLibrary::registerHandlers()
{
    bridge_.addHandler("musiclibrary.execSql", [this](const QJsonArray &a, Bridge::Reply reply) {
        QStringList statements;
        if (a.at(1).isArray())
            for (const auto statement : a.at(1).toArray())
                statements << statement.toString();
        else
            statements << stringArg(a, 1);
        db_.execute(this, statements, true, [this, id = a.at(0), reply](SqlResult r) {
            const auto command = id.toString().section('|', 0, 0);
            const auto &rows = command == "local init" || command == "getLocalMap"
                                   ? r.positionalRows
                                   : r.rows;
            const QJsonObject
                result{{"id", id}, {"result", bool(r)}, {"error", r.error}, {"value", rows}};
            bridge_.publish("musiclibrary.onexecsql", {result});
            reply({result});
        });
    });
    bridge_
        .addHandler("musiclibrary.readMusicInfo", [this](const QJsonArray &a, Bridge::Reply reply) {
            pool_.start([this, id = a.at(0), path = stringArg(a, 1), reply] {
                const auto info = MusicTags::read(path);
                QMetaObject::invokeMethod(this, [this, id, path, info, reply] {
                    const QJsonArray out{id, path, info ? 0 : 1, info.value_or(QJsonObject())};
                    bridge_.publish("musiclibrary.onreadmusicinfo", out);
                    reply(out);
                });
            });
        });
    bridge_.addHandler("musiclibrary.parseCueInfo", [this](const QJsonArray &a, Bridge::Reply reply) {
        pool_.start([this, id = a.at(0), path = stringArg(a, 1), reply] {
            const auto cue = CueParser::parse(path);
            QMetaObject::invokeMethod(this, [this, id, cue, reply] {
                const auto text
                    = QJsonDocument(cue.value_or(QJsonObject())).toJson(QJsonDocument::Compact);
                const QJsonArray
                    out{id, cue ? 0 : 1, QString::fromUtf8(text), cue ? QString() : cue.error()};
                bridge_.publish("musiclibrary.onparsecueinfo", out);
                reply(out);
            });
        });
    });
    bridge_.addHandler("musiclibrary.addLibrary", [this](const QJsonArray &a, Bridge::Reply reply) {
        scan(stringArg(a, 0), int(numberArg(a, 1, 50)));
        reply({});
    });
    bridge_
        .addHandler("musiclibrary.getLibraryPath", [this](const QJsonArray &a, Bridge::Reply reply) {
            QJsonArray paths;
            for (const auto library : a.at(0).isArray() ? a.at(0).toArray() : QJsonArray{a.at(0)}) {
                const auto key = library.toString();
                paths.append(QJsonObject{{"id", key}, {"path", resolveLibrary(key)}});
            }
            reply({paths});
        });
    bridge_
        .addHandler("musiclibrary.removeLibrary", [this](const QJsonArray &a, Bridge::Reply reply) {
            const auto key = stringArg(a, 0);
            cancel(key);
            db_.execute(
                this,
                {"DELETE FROM track WHERE dir=" + SqlDatabase::quote(key)},
                true,
                [this, key, reply](SqlResult r) {
                    refreshKnown();
                    const QJsonArray out{key, r.error, r.message};
                    bridge_.publish("musiclibrary.onremovelibrary", out);
                    reply(out);
                });
        });
    bridge_.addHandler(
        "musiclibrary.removeLibraryItems", [this](const QJsonArray &a, Bridge::Reply reply) {
            db_.run(
                this,
                [items = a.at(0).toArray()](SqlDatabase &db) {
                    QJsonArray failed, removed;
                    for (const auto value : items) {
                        const auto item = value.toObject();
                        const auto path = item["path"].toString();
                        if (item["removefile"].toBool() && QFile::exists(path)
                            && !QFile::remove(path)) {
                            failed.append(item);
                            continue;
                        }
                        const auto where = item["removebyid"].toBool()
                                               ? "id="
                                                     + SqlDatabase::quote(
                                                         item["id"].toVariant().toString())
                                               : "file=" + SqlDatabase::quote(path);
                        if (db.execute("UPDATE track SET ignored=1 WHERE " + where))
                            removed.append(item);
                        else
                            failed.append(item);
                    }
                    return QJsonObject{{"fail", failed}, {"success", removed}};
                },
                [this, reply](const QJsonObject &outcome) {
                    refreshKnown();
                    bridge_.publish("musiclibrary.onremovelibraryitems", {outcome});
                    reply({outcome});
                });
        });
    bridge_
        .addHandler("musiclibrary.observeLibrary", [this](const QJsonArray &a, Bridge::Reply reply) {
            const auto key = stringArg(a, 0), path = resolveLibrary(key);
            if (!path.isEmpty()) {
                observed_.insert(key);
                if (QFileInfo(path).isDir())
                    watcher_.addPath(path);
            }
            reply({!path.isEmpty()});
        });
    bridge_.addHandler(
        "musiclibrary.removeObserveLibrary", [this](const QJsonArray &a, Bridge::Reply reply) {
            const auto key = stringArg(a, 0);
            observed_.remove(key);
            if (const auto path = resolveLibrary(key); !path.isEmpty())
                watcher_.removePath(path);
            reply({true});
        });
    bridge_.addHandler(
        "musiclibrary.queryLibraryVersion", [this](const QJsonArray &, Bridge::Reply reply) {
            db_.execute(this, {"PRAGMA table_info(track)"}, false, [this, reply](SqlResult r) {
                QSet<QString> columns;
                for (const auto row : std::as_const(r.rows))
                    columns.insert(row.toObject()["name"].toString());
                const bool supported = r && columns.contains("file") && columns.contains("source")
                                       && columns.contains("starttime") && columns.contains("type")
                                       && columns.size() >= 22;
                const QJsonArray out{QJsonObject{{"result", supported}, {"need_update", false}}};
                bridge_.publish("musiclibrary.onquerylibraryversion", out);
                reply(out);
            });
        });
}

QString MusicLibrary::resolveLibrary(const QString &key) const
{
    if (key == "<mymusic>")
        return QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (key == "<download>")
        return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (key.startsWith('<'))
        return {};
    return key;
}

void MusicLibrary::cancel(const QString &key)
{
    if (const auto running = scans_.value(key))
        running->store(true);
}

void MusicLibrary::scan(const QString &key, int batchSize)
{
    cancel(key);
    const auto flag = std::make_shared<std::atomic_bool>(false);
    scans_[key] = flag;
    db_.run(
        this,
        [key](SqlDatabase &db) -> std::expected<Snapshot, QString> {
            const auto r = db.execute(
                "SELECT id,file,source,timestamp,filesize FROM track WHERE dir="
                + SqlDatabase::quote(key));
            if (!r)
                return std::unexpected(r.message);
            Snapshot snapshot;
            for (const auto value : r.rows) {
                const auto row = value.toObject();
                if (const auto sheet = row["source"].toString(); sheet.isEmpty())
                    snapshot.files.insert(row["file"].toString(), row);
                else
                    snapshot.sheets[sheet].append(row);
            }
            return snapshot;
        },
        [this, key, batchSize, flag](std::expected<Snapshot, QString> snapshot) {
            if (flag->load())
                reconcile(key, std::nullopt, flag, "cancel");
            else if (!snapshot)
                reconcile(key, std::nullopt, flag, snapshot.error());
            else
                walk(
                    key,
                    resolveLibrary(key),
                    qBound(1, batchSize <= 0 ? 50 : batchSize, 512),
                    flag,
                    std::move(*snapshot));
        });
}

void MusicLibrary::walk(
    const QString &key, const QString &root, int batchSize, const Cancel &flag, Snapshot snapshot)
{
    pool_.start([this, key, root, batchSize, flag, snapshot = std::move(snapshot)] {
        if (root.isEmpty() || !QFileInfo::exists(root)) {
            std::optional<QSet<QString>> seen;
            if (!root.isEmpty())
                seen.emplace();
            QMetaObject::invokeMethod(this, [this, key, flag, seen] {
                reconcile(key, seen, flag, missingLibrary);
            });
            return;
        }

        QSet<QString> folders{root};
        QList<QFileInfo> entries;
        QDirIterator
            it(root,
               QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
               QDirIterator::Subdirectories);
        while (it.hasNext() && !flag->load()) {
            const auto entry = it.nextFileInfo();
            if (entry.isDir()) {
                if (!entry.isSymLink())
                    folders.insert(entry.absoluteFilePath());
            } else if (isCue(entry) || MusicTags::supported(entry.filePath())) {
                entries << entry;
            }
        }

        QSet<QString> seen, sheetAudio;
        QHash<QString, QJsonObject> parsed;
        for (const auto &entry : std::as_const(entries)) {
            if (flag->load() || !isCue(entry))
                continue;
            const auto sheet = entry.canonicalFilePath();
            const auto stored = snapshot.sheets.value(sheet);
            if (!stored.isEmpty() && stored.first()["timestamp"].toInteger() == modified(entry)) {
                for (const auto &track : stored) {
                    seen.insert(track["id"].toString());
                    sheetAudio.insert(track["file"].toString());
                }
            } else if (const auto cue = CueParser::parse(sheet)) {
                for (const auto track : cue->value("tracks").toArray())
                    sheetAudio.insert(track.toObject()["audiopath"].toString());
                parsed.insert(sheet, *cue);
            }
        }

        QJsonArray batch;
        const auto permits = std::make_shared<QSemaphore>(4);
        const auto flush = [&] {
            if (batch.isEmpty())
                return;
            while (!permits->tryAcquire(1, 50))
                if (flag->load())
                    return;
            QMetaObject::invokeMethod(
                this, [this, key, rows = std::exchange(batch, {}), flag, permits] {
                    accept(key, rows, flag, [permits] { permits->release(); });
                });
        };
        for (const auto &entry : std::as_const(entries)) {
            if (flag->load())
                break;
            const auto path = entry.canonicalFilePath();
            QJsonArray rows;
            if (isCue(entry)) {
                const auto cue = parsed.constFind(path);
                if (cue == parsed.cend())
                    continue;
                for (const auto value : cue->value("tracks").toArray()) {
                    auto track = value.toObject();
                    track["file"] = track["audiopath"];
                    track["source"] = path;
                    track["starttime"] = track["start"];
                    if (track["id"].toString().isEmpty())
                        track["id"] = hash(
                            path
                            + "cueOffset=" + QString::number(qint64(track["start"].toDouble())));
                    track["timestamp"] = modified(entry);
                    rows.append(track);
                }
            } else if (!sheetAudio.contains(path)) {
                const auto stored = snapshot.files.value(path);
                if (!stored.isEmpty() && stored["timestamp"].toInteger() == modified(entry)
                    && stored["filesize"].toInteger() == entry.size()) {
                    seen.insert(stored["id"].toString());
                    continue;
                }
                auto tags = MusicTags::read(path);
                if (!tags || tags->value("duration").toDouble() < minimumDurationMs)
                    continue;
                (*tags)["id"] = hash(path);
                (*tags)["source"] = "";
                (*tags)["starttime"] = 0;
                (*tags)["type"] = 0;
                rows.append(*tags);
            }
            for (const auto value : std::as_const(rows)) {
                auto row = value.toObject();
                row["dir"] = key;
                row["librarypath"] = key;
                row["parentdir"] = entry.absolutePath();
                if (row["title"].toString().isEmpty())
                    row["title"] = QFileInfo(row["file"].toString()).completeBaseName();
                if (row.contains("index"))
                    row["tracknumber"] = row["index"];
                seen.insert(row["id"].toString());
                batch.append(row);
                if (batch.size() >= batchSize)
                    flush();
            }
        }
        flush();

        QMetaObject::invokeMethod(this, [this, key, flag, seen, folders] {
            if (observed_.contains(key)) {
                const auto watched = watcher_.directories();
                QStringList added;
                for (const auto &folder : folders)
                    if (!watched.contains(folder))
                        added << folder;
                if (!added.isEmpty())
                    watcher_.addPaths(added);
            }
            reconcile(key, seen, flag, {});
        });
    });
}

void MusicLibrary::accept(
    const QString &key, const QJsonArray &rows, const Cancel &flag, std::function<void()> done)
{
    if (flag->load() || scans_.value(key) != flag) {
        done();
        return;
    }
    db_.run(
        this,
        [rows, flag](SqlDatabase &db) {
            Accepted accepted;
            if (flag->load())
                return accepted;
            QStringList sql;
            for (const auto value : rows) {
                const auto row = value.toObject();
                const auto where = "id=" + SqlDatabase::quote(row["id"].toString())
                                   + " AND dir=" + SqlDatabase::quote(row["dir"].toString());
                const auto existing = db.execute("SELECT ignored FROM track WHERE " + where);
                if (existing.rows.isEmpty() || existing.rows[0].toObject()["ignored"].toInt() == 0) {
                    accepted.files << row["file"].toString();
                    if (const auto sheet = row["source"].toString(); !sheet.isEmpty())
                        accepted.files << sheet;
                }
                QStringList names, values, updates;
                for (const auto &column : scannedColumns) {
                    if (!row.contains(column))
                        continue;
                    const auto value = sqlValue(row[column]);
                    names << column;
                    values << value;
                    updates << column + "=" + value;
                }
                if (existing.rows.isEmpty()) {
                    ++accepted.added;
                    sql << "INSERT INTO track (" + names.join(',') + ") SELECT " + values.join(',')
                               + " WHERE NOT EXISTS (SELECT 1 FROM track WHERE " + where + ")";
                } else {
                    sql << "UPDATE track SET " + updates.join(',') + " WHERE " + where;
                }
            }
            accepted.result = db.execute(sql, true);
            if (!accepted.result)
                flag->store(true);
            return accepted;
        },
        [this, key, flag, done](const Accepted &accepted) {
            done();
            if (!accepted.result) {
                if (scans_.value(key) == flag)
                    scanErrors_[key] = accepted.result.message;
                return;
            }
            if (flag->load())
                return;
            for (const auto &file : accepted.files)
                knownFiles_.insert(file);
            if (accepted.added)
                bridge_.publish("musiclibrary.onaddprogress", {key, accepted.added});
        });
}

void MusicLibrary::reconcile(
    const QString &key, std::optional<QSet<QString>> seen, const Cancel &flag, const QString &error)
{
    if (scans_.value(key) != flag)
        return;
    db_.run(
        this,
        [key, seen = std::move(seen), flag, error](SqlDatabase &db) {
            if (flag->load())
                return SqlResult{1, "cancel"};
            if (!seen)
                return SqlResult{1, error};
            const auto library = SqlDatabase::quote(key);
            const auto stored = db.execute("SELECT id FROM track WHERE dir=" + library);
            if (!stored)
                return stored;
            QStringList gone;
            for (const auto row : stored.rows)
                if (const auto id = row.toObject()["id"].toString(); !seen->contains(id))
                    gone << "DELETE FROM track WHERE dir=" + library
                                + " AND id=" + SqlDatabase::quote(id);
            auto r = db.execute(gone, true);
            if (r && !error.isEmpty())
                r = SqlResult{1, error};
            return r;
        },
        [this, key, flag](const SqlResult &r) {
            if (scans_.value(key) != flag)
                return;
            scans_.remove(key);
            refreshKnown();
            const auto failure = scanErrors_.take(key);
            bridge_.publish(
                "musiclibrary.onaddend",
                {QJsonObject{{"library", key}, {"dirs", QJsonArray{resolveLibrary(key)}}},
                 r.error,
                 failure.isEmpty() ? r.message : failure});
        });
}

void MusicLibrary::refreshKnown()
{
    db_.run(
        this,
        [](SqlDatabase &db) -> std::optional<QSet<QString>> {
            const auto r = db.execute("SELECT file,source FROM track WHERE ignored=0");
            if (!r)
                return std::nullopt;
            QSet<QString> files;
            for (const auto value : r.rows) {
                const auto row = value.toObject();
                files.insert(row["file"].toString());
                if (const auto sheet = row["source"].toString(); !sheet.isEmpty())
                    files.insert(sheet);
            }
            return files;
        },
        [this](std::optional<QSet<QString>> files) {
            if (files)
                knownFiles_ = std::move(*files);
        });
}

}
