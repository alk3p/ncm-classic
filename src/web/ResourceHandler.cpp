#include "ResourceHandler.h"

#include "services/crypto/Crypto.h"
#include "services/library/MusicTags.h"
#include "services/persistence/Profile.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QUrlQuery>

namespace ncm {

namespace {

using saucer::scheme::error;
using Headers = std::map<std::string, std::string>;

constexpr qint64 maxRangeChunk = 8 * 1024 * 1024;
constexpr qsizetype maxHelperBody = 1024 * 1024;

QString header(const Headers &headers, QLatin1StringView name)
{
    for (const auto &[key, value] : headers) {
        const QLatin1StringView current(key.data(), qsizetype(key.size()));
        if (current.compare(name, Qt::CaseInsensitive) == 0)
            return QString::fromStdString(value);
    }
    return {};
}

QUrl initiator(const Headers &headers)
{
    const auto origin = header(headers, QLatin1StringView("Origin"));
    if (!origin.isEmpty() && origin != "null")
        return QUrl(origin);
    return QUrl(header(headers, QLatin1StringView("Referer")));
}

saucer::scheme::response reply(
    const QByteArray &bytes, const QByteArray &mime, Headers headers = {}, int status = 200)
{
    headers["Content-Length"] = std::to_string(bytes.size());
    return {
        .data = saucer::stash::from(std::vector<std::uint8_t>(bytes.cbegin(), bytes.cend())),
        .mime = mime.toStdString(),
        .headers = std::move(headers),
        .status = status,
    };
}

QByteArray mimeFor(const QString &path)
{
    if (path.endsWith(".js") || path.endsWith(".mjs"))
        return "application/javascript";
    if (path.endsWith(".less"))
        return "text/css";
    if (path.endsWith(".wasm"))
        return "application/wasm";
    return QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name().toLatin1();
}

QString embedded(const QString &root, const QString &relative)
{
    const auto prefix = root + '/';
    const auto path = QDir::cleanPath(prefix + relative);
    return path.startsWith(prefix) && QFile::exists(path) ? path : QString();
}

QString confined(const QString &root, const QString &relative)
{
    const auto base = QFileInfo(root).canonicalFilePath();
    const auto path = QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
    return !base.isEmpty() && path.startsWith(base + '/') ? path : QString();
}

QUrl unwrapped(const QUrl &url)
{
    if (url.host() != "cache" || url.path().size() > 1 || !url.hasQuery())
        return url;
    const QUrl inner(url.query(QUrl::FullyDecoded));
    return inner.scheme() == "orpheus" ? inner : QUrl();
}

QString localPath(const QString &decoded)
{
    static const QRegularExpression drive(QStringLiteral("^/[A-Za-z]:[/\\\\]"));
    return drive.match(decoded).hasMatch() ? decoded.mid(1) : decoded;
}

void serveFile(
    QFile &file, const QString &range, Headers headers, const saucer::scheme::executor &exec)
{
    static const QRegularExpression pattern(QStringLiteral("^bytes=(\\d*)-(\\d*)$"));
    const auto size = file.size();
    const auto mime = mimeFor(file.fileName());
    headers["Accept-Ranges"] = "bytes";
    const auto span = pattern.match(range.trimmed());
    if (!span.hasMatch()) {
        exec.resolve(reply(file.readAll(), mime, std::move(headers)));
        return;
    }

    qint64 begin = 0, end = size - 1;
    if (span.capturedLength(1)) {
        begin = span.captured(1).toLongLong();
        if (span.capturedLength(2))
            end = std::min(end, span.captured(2).toLongLong());
    } else if (span.capturedLength(2)) {
        begin = std::max<qint64>(0, size - span.captured(2).toLongLong());
    }
    if (begin >= size || begin > end) {
        headers["Content-Range"] = "bytes */" + std::to_string(size);
        exec.resolve(reply({}, mime, std::move(headers), 416));
        return;
    }
    end = std::min(end, begin + maxRangeChunk - 1);
    file.seek(begin);
    const auto bytes = file.read(end - begin + 1);
    headers["Content-Range"] = "bytes " + std::to_string(begin) + "-"
                               + std::to_string(begin + bytes.size() - 1) + "/"
                               + std::to_string(size);
    exec.resolve(reply(bytes, mime, std::move(headers), 206));
}

}

ResourceHandler::ResourceHandler(const RuntimePaths &paths)
    : paths_(paths)
{}

bool ResourceHandler::isAppOrigin(const QUrl &url)
{
    return url.scheme() == "orpheus" && (url.host() == "orpheus" || url.host() == "native");
}

void ResourceHandler::grantFile(const QString &path)
{
    grants_.insert(QFileInfo(path).canonicalFilePath());
}

bool ResourceHandler::isKnownFile(const QString &path) const
{
    const auto canonical = QFileInfo(path).canonicalFilePath();
    return !canonical.isEmpty()
           && (grants_.contains(canonical) || (knownFile_ && knownFile_(canonical)));
}

QString ResourceHandler::resolve(const QUrl &url) const
{
    const auto host = url.host();
    auto path = url.path();
    while (path.startsWith('/'))
        path.remove(0, 1);

    if (host == "orpheus" && path == "storage/local")
        return Profile(paths_).relativeFile(
            QUrlQuery(url).queryItemValue("file", QUrl::FullyDecoded));
    if (host == "orpheus")
        return embedded(":/ncm/assets/webapp", path);
    if (host == "native")
        return embedded(":/ncm/assets/webapp/pub", path);
    if (host == "skin") {
        if (path.startsWith("default/"))
            path.remove(0, 8);
        return embedded(":/ncm/assets/webapp/style/skin", path);
    }
    if (host == "cache" || host == "image" || host == "storage")
        return confined(paths_.storageCache, path);
    if (host == "localmusic") {
        const auto file = QFileInfo(localPath(url.path(QUrl::FullyDecoded))).canonicalFilePath();
        return isKnownFile(file) ? file : confined(paths_.downloadRoot, path);
    }
    return {};
}

void ResourceHandler::handle(
    const saucer::scheme::request &request,
    const saucer::scheme::executor &exec,
    bool pageTrusted) const
{
    const QUrl url(QString::fromStdString(request.url().string()));
    const auto host = url.host();
    const auto method = request.method();
    const auto headers = request.headers();
    const auto from = initiator(headers);
    const bool trustedCaller = from.isValid() ? isAppOrigin(from) : pageTrusted;
    Headers cors;
    if (from.isValid() && isAppOrigin(from))
        cors["Access-Control-Allow-Origin"] = ("orpheus://" + from.host()).toStdString();

    if (host == "orpheus" && url.path().startsWith("/__native/pure/")) {
        if (method != "POST" || !trustedCaller) {
            exec.reject(error::denied);
            return;
        }
        const auto body = request.content();
        const auto document = qsizetype(body.size()) <= maxHelperBody
                                  ? QJsonDocument::fromJson(
                                        QByteArray::fromRawData(
                                            reinterpret_cast<const char *>(body.data()),
                                            qsizetype(body.size())))
                                  : QJsonDocument();
        if (!document.isArray()) {
            exec.reject(error::invalid);
            return;
        }
        const auto value = crypto::pure(url.path().section('/', -1), document.array());
        const auto result = value ? QJsonObject{{"value", *value}}
                                  : QJsonObject{{"error", value.error()}};
        exec.resolve(
            reply(QJsonDocument(result).toJson(QJsonDocument::Compact), "application/json", cors));
        return;
    }

    static const QSet<QString> privateHosts{"cache", "image", "storage", "localmusic"};
    if ((privateHosts.contains(host) && !trustedCaller) || (method != "GET" && method != "HEAD")) {
        exec.reject(error::denied);
        return;
    }

    const auto target = unwrapped(url);
    if (!target.isValid()) {
        exec.reject(error::not_found);
        return;
    }
    if (isGenerated(target)) {
        auto content = generate(target);
        if (content)
            exec.resolve(reply(content->bytes, content->mime, std::move(cors)));
        else
            exec.reject(content.error());
        return;
    }
    const auto path = resolve(target);
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        exec.reject(error::not_found);
        return;
    }
    serveFile(file, header(headers, QLatin1StringView("Range")), std::move(cors), exec);
}

QByteArray ResourceHandler::read(const QUrl &url) const
{
    const auto target = unwrapped(url);
    if (isGenerated(target)) {
        auto content = generate(target);
        return content ? std::move(content->bytes) : QByteArray();
    }
    const auto path = resolve(target);
    QFile file(path);
    return !path.isEmpty() && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool ResourceHandler::isGenerated(const QUrl &url)
{
    const auto path = url.path();
    return (url.host() == "localmusic" && (path == "/lyric" || path == "/cover" || path == "/pic"))
           || (url.host() == "image" && QUrlQuery(url).hasQueryItem("path"));
}

std::expected<ResourceHandler::Generated, saucer::scheme::error> ResourceHandler::generate(
    const QUrl &url) const
{
    const QUrlQuery query(url);
    const auto path = url.host() == "image" ? query.queryItemValue("path", QUrl::FullyDecoded)
                                            : url.query(QUrl::FullyDecoded);
    if (!isKnownFile(path))
        return std::unexpected(error::denied);

    Generated content;
    if (url.path() == "/lyric") {
        if (const auto lyrics = MusicTags::lyrics(path))
            content = {lyrics->toUtf8(), "text/plain"};
    } else if (url.host() == "localmusic") {
        if (auto cover = MusicTags::cover(path))
            content = {std::move(cover->bytes), cover->mime.toLatin1()};
    } else {
        QImage image(path);
        const int width = query.queryItemValue("width").toInt();
        const int height = query.queryItemValue("height").toInt();
        if (width > 0 && height > 0)
            image = image.scaled(
                qMin(width, 4096),
                qMin(height, 4096),
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation);
        QBuffer encoded(&content.bytes);
        encoded.open(QIODevice::WriteOnly);
        image.save(&encoded, "PNG");
        content.mime = "image/png";
    }
    if (content.bytes.isEmpty())
        return std::unexpected(error::not_found);
    return content;
}

}
