#include "StreamCache.h"

#include "app/LegacyIdentity.h"
#include "app/RuntimePaths.h"
#include "bridge/Bridge.h"
#include "services/network/NativeNetwork.h"

#include <algorithm>
#include <optional>
#include <utility>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QTcpSocket>
#include <QUuid>

namespace ncm {

namespace {

constexpr qsizetype maxRequestHead = 64 * 1024;
constexpr qint64 chunk = 128 * 1024;
constexpr qint64 socketBuffer = 1024 * 1024;
constexpr qint64 replyBuffer = 2 * 1024 * 1024;
constexpr auto activePaths = "ncm.activeMediaPaths";
const auto metaSuffix = QStringLiteral(".cache.json");
const auto partSuffix = QStringLiteral(".part");

struct Span
{
    qint64 begin;
    qint64 end;
};

std::optional<Span> span(const QByteArray &range, qint64 size)
{
    if (range.isEmpty())
        return Span{0, size - 1};
    static const QRegularExpression pattern(QStringLiteral("^bytes=(\\d*)-(\\d*)$"));
    const auto match = pattern.match(QString::fromLatin1(range));
    if (!match.hasMatch() || (!match.capturedLength(1) && !match.capturedLength(2)))
        return std::nullopt;
    if (!match.capturedLength(1)) {
        const auto suffix = match.captured(2).toLongLong();
        if (suffix <= 0 || size == 0)
            return std::nullopt;
        return Span{std::max<qint64>(0, size - suffix), size - 1};
    }
    const auto begin = match.captured(1).toLongLong();
    const auto end = match.capturedLength(2) ? std::min(size - 1, match.captured(2).toLongLong())
                                             : size - 1;
    if (begin >= size || end < begin)
        return std::nullopt;
    return Span{begin, end};
}

void respond(QTcpSocket *socket, const QByteArray &status, const QByteArray &headers = {})
{
    socket->write(
        "HTTP/1.1 " + status + "\r\n" + headers
        + "Content-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    socket->disconnectFromHost();
}

void serveFile(QTcpSocket *socket, const QString &path, const QByteArray &range, bool headOnly)
{
    const auto file = std::make_shared<QFile>(path);
    if (!file->open(QIODevice::ReadOnly)) {
        respond(socket, "404 Not Found");
        return;
    }
    const auto size = file->size();
    const auto bytes = span(range, size);
    if (!bytes) {
        respond(
            socket,
            "416 Range Not Satisfiable",
            "Content-Range: bytes */" + QByteArray::number(size) + "\r\n");
        return;
    }

    const auto [begin, end] = *bytes;
    QByteArray head = "HTTP/1.1 200 OK\r\n";
    if (!range.isEmpty())
        head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + QByteArray::number(begin)
               + '-' + QByteArray::number(end) + '/' + QByteArray::number(size) + "\r\n";
    socket->write(
        head + "Accept-Ranges: bytes\r\nContent-Length: " + QByteArray::number(end - begin + 1)
        + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    if (headOnly || !file->seek(begin)) {
        socket->disconnectFromHost();
        return;
    }

    const auto feed = [socket, file, end] {
        while (file->pos() <= end && socket->bytesToWrite() < socketBuffer) {
            const auto data = file->read(std::min(chunk, end - file->pos() + 1));
            if (data.isEmpty()) {
                socket->abort();
                return;
            }
            socket->write(data);
        }
        if (file->pos() > end)
            socket->disconnectFromHost();
    };
    QObject::connect(socket, &QTcpSocket::bytesWritten, socket, feed);
    feed();
}

QString text(const QJsonObject &object, const QString &key)
{
    return object.value(key).toVariant().toString();
}

QJsonObject readEntry(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                          : QJsonObject();
}

bool complete(const QJsonObject &entry, const QString &path)
{
    const QFileInfo file(path);
    return entry.value("cached").toInt() == 100 && file.isFile()
           && entry.value("size").toInteger() == file.size()
           && (!entry.contains("mtimeMs")
               || entry.value("mtimeMs").toInteger() == file.lastModified().toMSecsSinceEpoch());
}

}

struct StreamCache::Request
{
    QByteArray method;
    QByteArray range;
};

struct StreamCache::Source
{
    QString url;
    QString path;
    QJsonObject info;
    QList<QPointer<QTcpSocket>> clients;
};

struct StreamCache::Transfer
{
    std::shared_ptr<Source> source;
    QPointer<QTcpSocket> socket;
    QPointer<QNetworkReply> reply;
    QFile part;
    qint64 offset = 0;
    qint64 written = 0;
    qint64 total = -1;
    bool caching = false;
    bool started = false;
};

StreamCache::StreamCache(
    Bridge &bridge, NativeNetwork &network, const RuntimePaths &paths, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , network_(network)
    , defaultRoot_(paths.storageCache)
    , settingsFile_(paths.settingsFile)
{
    server_.setProxy(QNetworkProxy::NoProxy);
    server_.listen(QHostAddress::LocalHost);
    connect(&server_, &QTcpServer::newConnection, this, &StreamCache::accept);
}

StreamCache::~StreamCache()
{
    while (!sources_.isEmpty())
        drop(sources_.begin().key());
}

QUrl StreamCache::playerPage() const
{
    return endpoint(QStringLiteral("player"));
}

QUrl StreamCache::stream(const QString &url, const QJsonObject &info)
{
    const auto song = text(info, "songId");
    const auto key = QCryptographicHash::hash(
        (song + '|' + text(info, "bitrate") + '|' + text(info, "md5") + '|'
         + (song.isEmpty() ? url : QString()))
            .toUtf8(),
        QCryptographicHash::Sha256);
    static const QRegularExpression suffix(QStringLiteral("^[A-Za-z0-9]{1,16}$"));
    auto format = info.value("format").toString();
    if (!suffix.match(format).hasMatch())
        format = QStringLiteral("audio");
    const auto root = cacheRoot();
    QDir().mkpath(root);
    return add(
        std::make_shared<Source>(Source{
            .url = url,
            .path = root + '/' + QString::fromLatin1(key.toHex()) + '.' + format,
            .info = info,
        }));
}

QUrl StreamCache::file(const QString &path)
{
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable())
        return {};
    return add(std::make_shared<Source>(Source{.path = file.absoluteFilePath()}));
}

void StreamCache::release(const QUrl &url)
{
    drop(url.path().mid(1));
}

QString StreamCache::cached(const QJsonObject &info) const
{
    const auto song = text(info, "songId");
    if (song.isEmpty())
        return {};
    const auto md5 = text(info, "md5");
    const auto bitrate = info.value("bitrate").toVariant().toInt();
    const QDir root(cacheRoot());
    for (const auto &name : root.entryList({"*" + metaSuffix}, QDir::Files)) {
        const auto entry = readEntry(root.filePath(name));
        const auto path = entry.value("path").toString();
        if (text(entry, "songId") == song && (md5.isEmpty() || text(entry, "md5") == md5)
            && (bitrate == 0 || entry.value("bitrate").toVariant().toInt() == bitrate)
            && complete(entry, path))
            return path;
    }
    return {};
}

QUrl StreamCache::endpoint(const QString &name) const
{
    if (!server_.isListening())
        return {};
    return QUrl(QStringLiteral("http://127.0.0.1:%1/%2").arg(server_.serverPort()).arg(name));
}

QUrl StreamCache::add(std::shared_ptr<Source> source)
{
    const auto url = endpoint(QUuid::createUuid().toString(QUuid::Id128));
    if (url.isEmpty())
        return {};
    setActive(source->path, true);
    sources_.insert(url.path().mid(1), std::move(source));
    return url;
}

void StreamCache::drop(const QString &token)
{
    const auto source = sources_.take(token);
    if (!source)
        return;
    setActive(source->path, false);
    for (const auto &client : source->clients)
        if (client)
            client->abort();
}

void StreamCache::accept()
{
    while (auto *socket = server_.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            const auto input = socket->peek(maxRequestHead);
            const auto end = input.indexOf("\r\n\r\n");
            if (end < 0) {
                if (input.size() == maxRequestHead)
                    socket->abort();
                return;
            }
            socket->disconnect(this);
            socket->read(end + 4);
            serve(socket, input.left(end));
        });
    }
}

void StreamCache::serve(QTcpSocket *socket, const QByteArray &head)
{
    const auto lines = head.split('\n');
    const auto words = lines.value(0).trimmed().split(' ');
    Request request{.method = words.value(0)};
    for (const auto &line : lines.mid(1))
        if (const auto colon = line.indexOf(':');
            colon > 0 && line.left(colon).trimmed().toLower() == "range")
            request.range = line.mid(colon + 1).trimmed();
    if (request.method != "GET" && request.method != "HEAD") {
        respond(socket, "405 Method Not Allowed");
        return;
    }

    const auto target = QString::fromLatin1(words.value(1)).section('?', 0, 0);
    if (target == "/player") {
        socket->write(
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: 56\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
            "<!doctype html><meta charset=utf-8><title>Player</title>");
        socket->disconnectFromHost();
        return;
    }
    const auto source = sources_.value(target.mid(1));
    if (!source) {
        respond(socket, "404 Not Found");
        return;
    }
    source->clients.removeAll(nullptr);
    source->clients.append(socket);
    if (source->url.isEmpty() || chksum(*source))
        serveFile(socket, source->path, request.range, request.method == "HEAD");
    else
        relay(socket, source, request);
}

bool StreamCache::chksum(const Source &source)
{
    const auto metaPath = source.path + metaSuffix;
    const auto entry = readEntry(metaPath);
    if (entry.isEmpty())
        return false;
    if (complete(entry, source.path) && text(entry, "songId") == text(source.info, "songId")
        && text(entry, "md5") == text(source.info, "md5"))
        return true;
    if (!writing_.contains(source.path)) {
        QFile::remove(source.path);
        QFile::remove(metaPath);
    }
    return false;
}

void StreamCache::relay(
    QTcpSocket *socket, const std::shared_ptr<Source> &source, const Request &request)
{
    NativeNetwork::Headers
        headers{{"User-Agent", legacy::streamUserAgent}, {"Accept-Encoding", "identity"}};
    if (!request.range.isEmpty())
        headers.append({"Range", request.range});
    const auto sent = network_.send(request.method, QUrl(source->url), {}, headers);
    if (!sent) {
        respond(socket, "502 Bad Gateway");
        return;
    }

    const auto transfer = std::make_shared<Transfer>();
    transfer->source = source;
    transfer->socket = socket;
    transfer->reply = *sent;
    transfer->reply->setReadBufferSize(replyBuffer);

    static const QRegularExpression openEnded(QStringLiteral("^bytes=(\\d+)-$"));
    const auto match = openEnded.match(QString::fromLatin1(request.range));
    transfer->offset = match.hasMatch() ? match.captured(1).toLongLong() : 0;
    const auto partPath = source->path + partSuffix;
    if (request.method == "GET" && (request.range.isEmpty() || match.hasMatch())
        && !writing_.contains(source->path)
        && (transfer->offset == 0 || QFileInfo(partPath).size() == transfer->offset)) {
        transfer->part.setFileName(partPath);
        const auto mode = transfer->offset ? QIODevice::Append : QIODevice::Truncate;
        if (transfer->part.open(QIODevice::WriteOnly | mode)) {
            transfer->caching = true;
            transfer->written = transfer->offset;
            writing_.insert(source->path);
        }
    }

    auto *reply = transfer->reply.data();
    connect(reply, &QNetworkReply::readyRead, this, [this, transfer] { forward(transfer); });
    connect(reply, &QNetworkReply::finished, this, [this, transfer] { forward(transfer); });
    connect(socket, &QTcpSocket::bytesWritten, reply, [this, transfer] { forward(transfer); });
    connect(socket, &QTcpSocket::disconnected, reply, &QNetworkReply::abort);
}

void StreamCache::forward(const std::shared_ptr<Transfer> &transfer)
{
    auto *reply = transfer->reply.data();
    if (!reply)
        return;
    const bool connected = transfer->socket && transfer->socket->isOpen();
    if (connected && !transfer->started && !begin(*transfer))
        return;
    while (connected && reply->bytesAvailable() && transfer->socket->bytesToWrite() < socketBuffer) {
        const auto data = reply->read(chunk);
        transfer->socket->write(data);
        if (!transfer->caching)
            continue;
        if (transfer->part.write(data) == data.size())
            transfer->written += data.size();
        else
            stop(*transfer);
    }
    if (reply->isFinished() && (!connected || !reply->bytesAvailable()))
        finish(transfer);
}

bool StreamCache::begin(Transfer &transfer)
{
    auto *reply = transfer.reply.data();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!status && !reply->isFinished())
        return false;
    transfer.started = true;
    if (!status) {
        stop(transfer);
        respond(transfer.socket, "502 Bad Gateway");
        return true;
    }

    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' '
                      + reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toByteArray()
                      + "\r\n";
    for (const auto name : {"Content-Length", "Content-Range", "Accept-Ranges"})
        if (reply->hasRawHeader(name))
            head += QByteArray(name) + ": " + reply->rawHeader(name) + "\r\n";
    transfer.socket->write(head + "Cache-Control: no-store\r\nConnection: close\r\n\r\n");

    if (status == 206) {
        static const QRegularExpression range(QStringLiteral("^bytes (\\d+)-\\d+/(\\d+)$"));
        const auto match = range.match(QString::fromLatin1(reply->rawHeader("Content-Range")));
        if (match.hasMatch() && match.captured(1).toLongLong() == transfer.offset)
            transfer.total = match.captured(2).toLongLong();
    } else if (status == 200 && transfer.offset == 0) {
        transfer.total = reply->rawHeader("Content-Length").toLongLong();
    }
    if (transfer.total <= 0)
        stop(transfer);
    return true;
}

void StreamCache::finish(const std::shared_ptr<Transfer> &transfer)
{
    QNetworkReply *reply = transfer->reply;
    transfer->reply.clear();
    reply->disconnect(this);
    if (transfer->caching) {
        const bool whole = reply->error() == QNetworkReply::NoError
                           && transfer->written == transfer->total;
        stop(*transfer);
        if (whole)
            commit(*transfer->source, transfer->total);
    }
    if (transfer->socket)
        transfer->socket->disconnectFromHost();
    reply->deleteLater();
    trim();
}

void StreamCache::stop(Transfer &transfer)
{
    if (!std::exchange(transfer.caching, false))
        return;
    transfer.part.close();
    writing_.remove(transfer.source->path);
}

void StreamCache::commit(const Source &source, qint64 size)
{
    QFile::remove(source.path);
    if (!QFile::rename(source.path + partSuffix, source.path))
        return;
    auto entry = source.info;
    entry.insert("cached", 100);
    entry.insert("path", source.path);
    entry.insert("cacheurl", source.path);
    entry.insert("size", size);
    entry.insert("mtimeMs", QFileInfo(source.path).lastModified().toMSecsSinceEpoch());
    QSaveFile meta(source.path + metaSuffix);
    if (meta.open(QIODevice::WriteOnly)) {
        meta.write(QJsonDocument(entry).toJson(QJsonDocument::Compact));
        meta.commit();
    }
}

void StreamCache::trim()
{
    const QSettings settings(settingsFile_, QSettings::IniFormat);
    const auto capacity = qint64(
        settings.value("ServiceStorage/cache-capacity", 1).toDouble() * 1024 * 1024 * 1024);
    const QDir root(cacheRoot());
    const auto order = QDir::Time | QDir::Reversed;
    const auto parts = root.entryInfoList({"*" + partSuffix}, QDir::Files, order);
    const auto metas = root.entryInfoList({"*" + metaSuffix}, QDir::Files, order);
    const auto active = bridge_.property(activePaths).toStringList();
    const auto spared = [&](const QString &song) {
        return active.contains(song) || writing_.contains(song);
    };

    qint64 size = 0;
    for (const auto &part : parts)
        size += part.size();
    for (const auto &meta : metas)
        size += QFileInfo(meta.filePath().chopped(metaSuffix.size())).size();

    for (const auto &part : parts) {
        if (size <= capacity)
            return;
        if (!spared(part.filePath().chopped(partSuffix.size())) && QFile::remove(part.filePath()))
            size -= part.size();
    }
    for (const auto &meta : metas) {
        if (size <= capacity)
            return;
        const QFileInfo song(meta.filePath().chopped(metaSuffix.size()));
        if (spared(song.filePath()))
            continue;
        const auto bytes = song.size();
        if (!song.exists() || QFile::remove(song.filePath())) {
            QFile::remove(meta.filePath());
            size -= bytes;
        }
    }
}

QString StreamCache::cacheRoot() const
{
    const QSettings settings(settingsFile_, QSettings::IniFormat);
    return QDir(settings.value("ServiceStorage/cache-path", defaultRoot_).toString()).absolutePath();
}

void StreamCache::setActive(const QString &path, bool active)
{
    auto paths = bridge_.property(activePaths).toStringList();
    if (active)
        paths.append(path);
    else
        paths.removeOne(path);
    bridge_.setProperty(activePaths, paths);
}

}
