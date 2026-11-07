#include "StreamCache.h"

#include "app/RuntimePaths.h"
#include "bridge/Bridge.h"
#include "services/network/NativeNetwork.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QTcpSocket>
#include <QUuid>

namespace ncm {

struct StreamCache::Entry
{
    QString url, path, part, key;
    QJsonObject info;
    int users = 0;
    qint64 covered = 0, total = -1;
    QList<QPointer<QTcpSocket>> clients;
    QList<QPointer<QNetworkReply>> replies;
};

StreamCache::StreamCache(Bridge &b, NativeNetwork &n, const RuntimePaths &p, QObject *parent)
    : QObject(parent)
    , network_(n)
    , bridge_(b)
    , server_(this)
    , root_(p.storageCache)
    , settings_(p.settingsFile)
{
    QSettings s(settings_, QSettings::IniFormat);
    root_ = s.value("ServiceStorage/cache-path", root_).toString();
    QDir().mkpath(root_);
    server_.listen(QHostAddress::LocalHost);
    connect(&server_, &QTcpServer::newConnection, this, &StreamCache::accept);
}

StreamCache::~StreamCache()
{
    const auto keys = entries_.keys();
    for (const auto &key : keys)
        release(QUrl("http://127.0.0.1/" + key));
    server_.close();
}

QUrl StreamCache::source(const QString &url, const QJsonObject &info)
{
    QSettings settings(settings_, QSettings::IniFormat);
    root_ = settings.value("ServiceStorage/cache-path", root_).toString();
    QDir().mkpath(root_);
    if (!server_.isListening())
        return QUrl(url);
    const QString song = info.value("songId").toVariant().toString(),
                  md5 = info.value("md5").toString();
    const QString bitrate = info.value("bitrate").toVariant().toString();
    const auto key = QString::fromLatin1(
        QCryptographicHash::hash(
            (song + "|" + bitrate + "|" + md5 + "|" + (song.isEmpty() ? url : QString())).toUtf8(),
            QCryptographicHash::Sha256)
            .toHex());
    auto e = std::make_shared<Entry>();
    e->key = key;
    e->url = url;
    e->path = root_ + "/" + key + "." + info.value("format").toString("audio");
    e->part = e->path + ".part";
    e->info = info;
    e->users = 1;
    const auto token = QUuid::createUuid().toString(QUuid::Id128);
    entries_.insert(token, e);
    auto paths = bridge_.property("ncm.activeMediaPaths").toStringList();
    paths.append(e->path);
    bridge_.setProperty("ncm.activeMediaPaths", paths);
    return QUrl(QString("http://127.0.0.1:%1/%2").arg(server_.serverPort()).arg(token));
}

double StreamCache::bufferedFraction(const QUrl &url) const
{
    const auto e = entries_.value(url.path().mid(1));
    return e && e->total > 0 ? qBound(0., double(e->covered) / e->total, 1.) : 0.;
}

void StreamCache::release(const QUrl &url)
{
    const auto token = url.path().mid(1);
    auto e = entries_.value(token);
    if (!e)
        return;
    e->users = 0;
    for (auto r : e->replies)
        if (r)
            r->abort();
    for (auto s : e->clients)
        if (s)
            s->abort();
    auto paths = bridge_.property("ncm.activeMediaPaths").toStringList();
    paths.removeOne(e->path);
    bridge_.setProperty("ncm.activeMediaPaths", paths);
    entries_.remove(token);
    trim();
}

void StreamCache::accept()
{
    while (server_.hasPendingConnections()) {
        auto socket = server_.nextPendingConnection();
        socket->setReadBufferSize(65536);
        auto input = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, input] {
            *input += socket->readAll();
            if (!input->contains("\r\n\r\n"))
                return;
            socket->disconnect(this);
            const auto lines = input->split('\n');
            const auto words = lines.value(0).trimmed().split(' ');
            const auto token = QString::fromLatin1(words.value(1)).section('?', 0, 0).mid(1);
            auto e = entries_.value(token);
            if (!e || (words.value(0) != "GET" && words.value(0) != "HEAD")) {
                socket->write(
                    "HTTP/1.1 404 Not Found\r\nContent-Length: "
                    "0\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }
            e->clients.append(socket);
            QByteArray range;
            for (auto line : lines) {
                if (line.toLower().startsWith("range:"))
                    range = line.mid(6).trimmed();
            }
            qint64 offset = 0;
            auto match
                = QRegularExpression("^bytes=(\\d+)-(\\d*)$").match(QString::fromLatin1(range));
            if (match.hasMatch())
                offset = match.captured(1).toLongLong();
            bool validCache = false;
            if (QFileInfo::exists(e->path) && QFileInfo::exists(e->path + ".cache.json")) {
                QFile metaFile(e->path + ".cache.json");
                if (metaFile.open(QIODevice::ReadOnly)) {
                    const auto meta = QJsonDocument::fromJson(metaFile.readAll()).object();
                    const QFileInfo payload(e->path);
                    validCache = meta.value("cached").toInt() == 100
                                 && meta.value("size").toVariant().toLongLong() == payload.size()
                                 && meta.value("songId").toVariant().toString()
                                        == e->info.value("songId").toVariant().toString()
                                 && meta.value("md5").toString() == e->info.value("md5").toString();
                    if (meta.contains("mtimeMs"))
                        validCache = validCache
                                     && meta.value("mtimeMs").toVariant().toLongLong()
                                            == payload.lastModified().toMSecsSinceEpoch();
                }
                if (!validCache && !writing_.contains(e->path)) {
                    QFile::remove(e->path);
                    QFile::remove(e->path + ".cache.json");
                }
            }
            if (validCache) {
                auto file = std::make_shared<QFile>(e->path);
                if (file->open(QIODevice::ReadOnly)) {
                    e->covered = e->total = file->size();
                    if (!range.isEmpty() && !match.hasMatch()) {
                        auto suffix = QRegularExpression("^bytes=-(\\d+)$")
                                          .match(QString::fromLatin1(range));
                        if (suffix.hasMatch() && suffix.captured(1).toLongLong() > 0)
                            offset = qMax(qint64(0), file->size() - suffix.captured(1).toLongLong());
                        else
                            offset = file->size();
                    }
                    if (offset >= file->size()
                        || (match.hasMatch() && !match.captured(2).isEmpty()
                            && match.captured(2).toLongLong() < offset)) {
                        socket->write(
                            "HTTP/1.1 416 Range Not "
                            "Satisfiable\r\nContent-Range: bytes */"
                            + QByteArray::number(file->size())
                            + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }

                    qint64 end = file->size() - 1;
                    if (match.hasMatch() && !match.captured(2).isEmpty())
                        end = qMin(end, match.captured(2).toLongLong());
                    const auto count = qMax(qint64(0), end - offset + 1);
                    QByteArray header
                        = range.isEmpty()
                              ? QByteArray("HTTP/1.1 200 OK\r\n")
                              : "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                                    + QByteArray::number(offset) + "-" + QByteArray::number(end)
                                    + "/" + QByteArray::number(file->size()) + "\r\n";
                    const auto mime
                        = file->peek(4) == "fLaC"
                              ? QByteArray("audio/flac")
                              : QMimeDatabase().mimeTypeForFile(e->path).name().toLatin1();
                    header += "Content-Type: " + mime + "\r\n";
                    socket->write(
                        header + "Content-Length: " + QByteArray::number(count)
                        + "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n");
                    if (words.value(0) == "HEAD") {
                        socket->disconnectFromHost();
                        return;
                    }
                    file->seek(offset);
                    auto left = std::make_shared<qint64>(count);
                    auto pump = std::make_shared<std::function<void()>>();
                    QPointer<QTcpSocket> weak = socket;
                    *pump = [weak, file, left] {
                        if (!weak)
                            return;
                        while (*left > 0 && weak->bytesToWrite() < 512 * 1024) {
                            auto bytes = file->read(qMin(qint64(128 * 1024), *left));
                            if (bytes.isEmpty())
                                break;
                            weak->write(bytes);
                            *left -= bytes.size();
                        }
                        if (*left == 0)
                            weak->disconnectFromHost();
                    };
                    connect(socket, &QTcpSocket::bytesWritten, socket, [pump](qint64) {
                        (*pump)();
                    });
                    (*pump)();
                    return;
                }
            }
            struct Transfer
            {
                QPointer<QTcpSocket> socket;
                QPointer<QNetworkReply> reply;
                QFile cache;
                bool headers = false, caching = false, done = false;
                bool writer = false;
                qint64 down = 0, total = -1, offset = 0;
            };
            NativeNetwork::Headers headers{{"Accept-Encoding", "gzip, deflate"}};
            for (auto line : lines) {
                const auto colon = line.indexOf(':');
                if (colon <= 0)
                    continue;
                auto key = line.left(colon).trimmed(), value = line.mid(colon + 1).trimmed();
                const auto lower = key.toLower();
                if (lower == "user-agent" || lower == "accept" || lower == "accept-language"
                    || lower == "range" || lower == "icy-metadata" || lower == "accept-encoding")
                    headers.append({key, value});
            }
            auto sent = network_.send(words.value(0), QUrl(e->url), {}, headers);
            if (!sent) {
                socket->write(
                    "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }
            auto r = *sent;
            r->setReadBufferSize(2 * 1024 * 1024);
            auto t = std::make_shared<Transfer>();
            t->socket = socket;
            t->offset = offset;
            t->reply = r;
            t->cache.setFileName(e->part);
            if ((range.isEmpty() || match.hasMatch()) && !writing_.contains(e->path)
                && (offset == 0
                    || (QFileInfo::exists(e->part) && QFileInfo(e->part).size() == offset))) {
                t->caching = t->cache.open(
                    QIODevice::WriteOnly | (offset == 0 ? QIODevice::Truncate : QIODevice::Append));
                t->down = offset;
                if (t->caching) {
                    t->writer = true;
                    writing_.insert(e->path);
                }
            }
            e->replies.append(r);
            connect(socket, &QTcpSocket::disconnected, r, [t] {
                if (t->reply)
                    t->reply->abort();
            });
            auto read = [t, e] {
                auto r = t->reply.data();
                if (!r)
                    return;
                if (!t->socket) {
                    t->caching = false;
                    r->readAll();
                    return;
                }
                if (!t->headers) {
                    const int status
                        = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    if (status == 0)
                        return;
                    QByteArray header
                        = "HTTP/1.1 " + QByteArray::number(status) + " "
                          + r->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toByteArray()
                          + "\r\n";
                    const bool flac
                        = e->info.value("format").toString().compare("flac", Qt::CaseInsensitive)
                              == 0
                          || (t->offset == 0 && r->peek(4) == "fLaC");
                    for (auto h : r->rawHeaderPairs())
                        if (h.first.toLower() != "connection"
                            && h.first.toLower() != "transfer-encoding"
                            && (!flac || h.first.toLower() != "content-type"))
                            header += h.first + ": " + h.second + "\r\n";
                    if (flac)
                        header += "Content-Type: audio/flac\r\n";
                    t->socket->write(header + "Connection: close\r\n\r\n");
                    t->headers = true;
                    if (status == 200 && t->offset > 0) {
                        t->caching = false;
                        t->cache.close();
                    }
                    if (status == 206) {
                        auto m = QRegularExpression("^bytes (\\d+)-(\\d+)/(\\d+)$")
                                     .match(QString::fromLatin1(r->rawHeader("Content-Range")));
                        if (m.hasMatch() && m.captured(1).toLongLong() == t->offset)
                            t->total = m.captured(3).toLongLong();
                        else
                            t->caching = false;
                    } else if (status == 200 && r->hasRawHeader("Content-Length"))
                        t->total = r->rawHeader("Content-Length").toLongLong();
                    else
                        t->caching = false;
                    if (t->total > 0)
                        e->total = t->total;
                }
                while (r->bytesAvailable() && t->socket->bytesToWrite() < 1024 * 1024) {
                    auto data = r->read(128 * 1024);
                    if (data.isEmpty())
                        break;
                    t->socket->write(data);
                    if (t->caching) {
                        if (t->cache.write(data) != data.size())
                            t->caching = false;
                        else {
                            t->down += data.size();
                            e->covered = t->down;
                        }
                    }
                }
            };
            connect(r, &QNetworkReply::readyRead, this, read);
            connect(socket, &QTcpSocket::bytesWritten, r, [read](qint64) { read(); });
            auto finalize = [this, e, t, r, read] {
                if (t->done)
                    return;
                read();
                if (!r->isFinished() || r->bytesAvailable() > 0)
                    return;
                t->done = true;
                if (t->writer)
                    writing_.remove(e->path);
                const bool complete = t->caching && r->error() == QNetworkReply::NoError
                                      && t->total > 0 && t->down == t->total
                                      && t->cache.size() == t->total;
                if (t->cache.isOpen())
                    t->cache.flush();
                t->cache.close();
                if (complete && QFile::rename(e->part, e->path)) {
                    auto meta = e->info;
                    meta.insert("cached", 100);
                    meta.insert("path", e->path);
                    meta.insert("cacheurl", e->path);
                    meta.insert("size", t->total);
                    meta.insert("mtimeMs", QFileInfo(e->path).lastModified().toMSecsSinceEpoch());
                    QSaveFile f(e->path + ".cache.json");
                    if (f.open(QIODevice::WriteOnly)) {
                        f.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
                        f.commit();
                    }
                }
                if (t->socket)
                    t->socket->disconnectFromHost();
                r->deleteLater();
                trim();
            };
            connect(r, &QNetworkReply::finished, this, finalize);
            connect(socket, &QTcpSocket::bytesWritten, r, [finalize](qint64) { finalize(); });
        });
    }
}

void StreamCache::trim()
{
    QSettings settings(settings_, QSettings::IniFormat);
    const auto capacity = qMax(
        qint64(0),
        qint64(settings.value("ServiceStorage/cache-capacity", 1).toDouble() * 1024 * 1024 * 1024));
    QDir d(root_);
    const auto files = d.entryInfoList({"*.cache.json"}, QDir::Files, QDir::Time | QDir::Reversed);
    qint64 size = 0;
    const auto partials = d.entryInfoList({"*.part"}, QDir::Files, QDir::Time | QDir::Reversed);
    for (const auto &file : partials)
        size += file.size();
    for (const auto &f : files)
        size += QFileInfo(f.filePath().chopped(11)).size();
    const auto active = bridge_.property("ncm.activeMediaPaths").toStringList();
    for (const auto &f : partials) {
        if (size <= capacity)
            break;
        const auto finalPath = f.filePath().chopped(5);
        if (active.contains(finalPath) || writing_.contains(finalPath))
            continue;
        if (QFile::remove(f.filePath()))
            size -= f.size();
    }
    for (const auto &f : files) {
        if (size <= capacity)
            break;
        const auto path = f.filePath().chopped(11);
        if (active.contains(path))
            continue;
        const auto bytes = QFileInfo(path).size();
        if (QFile::remove(path)) {
            QFile::remove(f.filePath());
            size -= bytes;
        }
    }
}

}
