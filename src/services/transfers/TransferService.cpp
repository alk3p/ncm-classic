#include "TransferService.h"

#include "app/RuntimePaths.h"
#include "bridge/Bridge.h"
#include "services/network/NativeNetwork.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>

namespace ncm {

namespace {

constexpr qint64 progressMs = 250;
constexpr int succeeded = 0;
constexpr int fileFailure = 2;
constexpr int networkFailure = -101;

QString numbered(const QString &path, int n)
{
    const QFileInfo file(path);
    const auto name = file.fileName();
    const auto dot = name.lastIndexOf('.');
    const auto stem = dot > 0 ? name.left(dot) : name;
    const auto suffix = dot > 0 ? name.mid(dot) : QString();
    return file.dir().filePath(stem + QString("(%1)").arg(n) + suffix);
}

}

struct TransferService::Task
{
    QString id, url, md5;
    QString relative;
    QString target;
    QString partial;
    QFile file;
    QPointer<QNetworkReply> reply;
    QElapsedTimer clock;
    qint64 offset = 0;
    qint64 down = 0, total = -1;
    qint64 reported = 0;
    bool headers = false;
};

TransferService::TransferService(
    Bridge &bridge, NativeNetwork &network, const RuntimePaths &paths, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , network_(network)
    , downloadRoot_(paths.downloadRoot)
    , settings_(paths.settingsFile)
    , tempFiles_(paths.tempFiles)
{
    bridge.addHandler("download.start", [this](const QJsonArray &a, Bridge::Reply done) {
        start(a.at(0).toObject());
        done({});
    });
    bridge.addHandler("download.pause", [this](const QJsonArray &a, Bridge::Reply done) {
        stop(stringArg(a, 0), false);
        done({});
    });
    bridge.addHandler("download.cancel", [this](const QJsonArray &a, Bridge::Reply done) {
        stop(stringArg(a, 0), true);
        done({});
    });
    bridge.addHandler(
        "download.queryDownloadShecdule", [this](const QJsonArray &a, Bridge::Reply done) {
            QJsonArray schedule;
            for (const auto value : a.at(0).toArray()) {
                const auto item = value.toObject();
                qint64 down = 0, total = 0;
                if (const auto running = tasks_.find(item.value("id").toVariant().toString());
                    running != tasks_.end()) {
                    down = running->second->down;
                    total = running->second->total;
                } else {
                    const QSettings info(
                        partialPath(absolute(item.value("path").toString())) + ".info",
                        QSettings::IniFormat);
                    down = info.value("downloaded-size").toLongLong();
                    total = info.value("total-size").toLongLong();
                }
                schedule.append(
                    QJsonObject{
                        {"id", item.value("id")},
                        {"shecdule", total > 0 ? double(down) / total : 0}});
            }
            bridge_.publish("download.ondownloadeshecdule", {schedule});
            done({});
        });
}

TransferService::~TransferService()
{
    for (const auto &[_, task] : tasks_) {
        detach(*task);
        task->file.close();
        save(*task);
    }
}

void TransferService::start(const QJsonObject &options)
{
    const auto id = options.value("id").toVariant().toString();
    if (id.isEmpty())
        return;
    stop(id, false);

    auto task = std::make_unique<Task>();
    auto &t = *task;
    t.id = id;
    t.url = options.value("url").toString();
    t.md5 = options.value("md5").toString();
    t.relative = options.value("rel_path").toString();
    t.total = options.value("size").toVariant().toLongLong();
    if (t.total <= 0)
        t.total = -1;
    t.target = absolute(t.relative);
    for (int n = 1; targeted(t.target); ++n)
        t.target = numbered(absolute(t.relative), n);
    t.partial = partialPath(t.target);
    if (t.relative.isEmpty() || !QDir().mkpath(tempFiles_)
        || !QDir().mkpath(QFileInfo(t.target).absolutePath())) {
        report(t, true, fileFailure);
        return;
    }

    const QSettings info(t.partial + ".info", QSettings::IniFormat);
    if (QFileInfo(t.partial).size() == info.value("downloaded-size", -1).toLongLong()) {
        if (t.total < 0)
            t.total = info.value("total-size", -1).toLongLong();
    } else {
        QFile::remove(t.partial);
    }
    t.file.setFileName(t.partial);
    if (!t.file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        report(t, true, fileFailure);
        return;
    }
    t.offset = t.down = t.file.size();
    t.clock.start();
    tasks_.emplace(id, std::move(task));
    request(t);
}

void TransferService::stop(const QString &id, bool cancel)
{
    const auto running = tasks_.find(id);
    if (running == tasks_.end()) {
        if (cancel)
            discard(id);
        return;
    }
    auto &t = *running->second;
    detach(t);
    t.file.close();
    if (cancel) {
        QFile::remove(t.partial);
        QFile::remove(t.partial + ".info");
        bridge_.publish("download.oncancel", {id});
    } else {
        save(t);
    }
    tasks_.erase(running);
}

void TransferService::request(Task &t)
{
    NativeNetwork::Headers headers{{"Accept-Encoding", "identity"}};
    if (t.offset > 0)
        headers.append({"Range", "bytes=" + QByteArray::number(t.offset) + "-"});
    const auto sent = network_.send("GET", QUrl(t.url), {}, headers);
    if (!sent) {
        finish(t, networkFailure);
        return;
    }
    auto *reply = *sent;
    t.reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, task = &t] { receive(*task); });
    connect(reply, &QNetworkReply::finished, this, [this, task = &t] { finished(*task); });
}

bool TransferService::receive(Task &t)
{
    auto *r = t.reply.data();
    if (!t.headers) {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status >= 300)
            return true;
        if (status == 200 && t.offset > 0) {
            if (!t.file.resize(0)) {
                finish(t, fileFailure);
                return false;
            }
            t.offset = t.down = 0;
        }
        if (status == 206) {
            static const QRegularExpression contentRange("^bytes (\\d+)-(\\d+)/(\\d+|\\*)$");
            const auto range = contentRange.match(
                QString::fromLatin1(r->rawHeader("Content-Range")));
            if (!range.hasMatch() || range.captured(1).toLongLong() != t.offset) {
                finish(t, networkFailure);
                return false;
            }
            if (range.captured(3) != "*")
                t.total = range.captured(3).toLongLong();
        } else if (r->hasRawHeader("Content-Length")) {
            t.total = r->rawHeader("Content-Length").toLongLong();
        }
        t.headers = true;
    }
    const auto bytes = r->readAll();
    if (t.file.write(bytes) != bytes.size()) {
        finish(t, fileFailure);
        return false;
    }
    t.down += bytes.size();
    if (t.clock.elapsed() - t.reported >= progressMs) {
        t.reported = t.clock.elapsed();
        save(t);
        report(t, false, succeeded);
    }
    return true;
}

void TransferService::finished(Task &t)
{
    if (!receive(t))
        return;
    const auto *r = t.reply.data();
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 416 && t.total > 0 && t.offset == t.total) {
        finish(t, succeeded);
        return;
    }
    const bool whole = r->error() == QNetworkReply::NoError && status >= 200 && status < 300
                       && (t.total < 0 || t.down == t.total);
    finish(t, whole ? succeeded : networkFailure);
}

void TransferService::finish(Task &t, int code)
{
    detach(t);
    if (!t.file.flush() && code == succeeded)
        code = fileFailure;
    t.file.close();
    if (code == succeeded) {
        auto target = t.target;
        for (int n = 1; QFileInfo::exists(target); ++n)
            target = numbered(t.target, n);
        if (QFile::rename(t.partial, target)) {
            QFile::remove(t.partial + ".info");
            t.target = target;
        } else {
            code = fileFailure;
        }
    }
    if (code != succeeded)
        save(t);
    report(t, true, code);
    const auto id = t.id;
    tasks_.erase(id);
}

void TransferService::detach(Task &t)
{
    if (auto *r = t.reply.data()) {
        disconnect(r, nullptr, this, nullptr);
        r->abort();
        r->deleteLater();
    }
    t.reply = nullptr;
}

void TransferService::discard(const QString &id) const
{
    const QDir folder(tempFiles_);
    for (const auto &name : folder.entryList({"*.info"}, QDir::Files)) {
        const auto info = folder.filePath(name);
        if (QSettings(info, QSettings::IniFormat).value("id").toString() != id)
            continue;
        QFile::remove(info.chopped(5));
        QFile::remove(info);
    }
}

void TransferService::save(const Task &t) const
{
    QSettings info(t.partial + ".info", QSettings::IniFormat);
    info.setValue("id", t.id);
    info.setValue("md5", t.md5);
    info.setValue("total-size", t.total);
    info.setValue("downloaded-size", t.down);
}

void TransferService::report(const Task &t, bool last, int type)
{
    const auto slash = std::max(t.relative.lastIndexOf('/'), t.relative.lastIndexOf('\\'));
    const auto path = QDir::isAbsolutePath(t.relative)
                          ? t.target
                          : t.relative.left(slash + 1) + QFileInfo(t.target).fileName();
    const auto elapsed = t.clock.isValid() ? t.clock.elapsed() : 0;
    bridge_.publish(
        "download.onprocess",
        {t.id,
         QJsonObject{
             {"type", type},
             {"down", t.down},
             {"total", std::max<qint64>(0, t.total)},
             {"speed", elapsed > 0 ? (t.down - t.offset) * 1000 / elapsed : 0},
             {"islast", last},
             {"path", path}}});
}

QString TransferService::absolute(const QString &destination) const
{
    if (QDir::isAbsolutePath(destination))
        return QDir::cleanPath(destination);
    const auto folder = QSettings(settings_, QSettings::IniFormat)
                            .value("ServiceStorage/download-path", downloadRoot_)
                            .toString();
    return QDir::cleanPath(QDir(folder).absoluteFilePath(destination));
}

QString TransferService::partialPath(const QString &target) const
{
    return tempFiles_ + "/"
           + QString::fromLatin1(
               QCryptographicHash::hash(target.toUtf8(), QCryptographicHash::Md5).toHex());
}

bool TransferService::targeted(const QString &target) const
{
    return std::ranges::any_of(tasks_, [&](const auto &task) {
        return task.second->target == target;
    });
}

}
