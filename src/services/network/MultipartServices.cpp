#include "MultipartServices.h"

#include "NativeNetwork.h"
#include "bridge/Bridge.h"
#include "services/crypto/Crypto.h"

#include <atomic>
#include <expected>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QUuid>

namespace ncm {

namespace {

QJsonObject response(QNetworkReply *reply)
{
    QJsonArray headers;
    for (auto header : reply->rawHeaderPairs())
        headers.append(
            QJsonArray{QString::fromLatin1(header.first), QString::fromLatin1(header.second)});
    auto data = NativeNetwork::readBody(reply);
    return {
        {"status", reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()},
        {"headers", headers},
        {"bodyBase64", QString::fromLatin1(data.toBase64())},
        {"error", int(reply->error())},
        {"errorString", reply->errorString()},
        {"finalUrl", reply->url().toString()},
        {"statusText", reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString()}};
}

QJsonObject responseError(QNetworkReply::NetworkError code, const QString &message)
{
    return {{"status", 0}, {"error", int(code)}, {"errorString", message}};
}

struct ImageSource
{
    QByteArray bytes;
    QString format, md5;
};

std::expected<ImageSource, QString> readImage(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(file.errorString());
    auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return std::unexpected(file.errorString());
    const auto mime = QMimeDatabase().mimeTypeForData(bytes).name();
    if (bytes.isEmpty() || !mime.startsWith("image/"))
        return std::unexpected(QStringLiteral("Upload source is not a readable image"));
    const auto md5 = QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex();
    return ImageSource{std::move(bytes), mime.section('/', 1), QString::fromLatin1(md5)};
}

}

struct MultipartServices::ImageTask
{
    QString id, path, format, account;
    QByteArray cookie, bytes;
    QJsonObject allocation;
    Completion done;
    std::atomic_bool active{true};
};

QString MultipartServices::account() const
{
    QUrl url("https://music.163.com/");
    return network_.cookieValue(url, "MUSIC_U") + "|" + network_.cookieValue(url, "MUSIC_A");
}

void MultipartServices::imageFinish(const std::shared_ptr<ImageTask> &task, QJsonObject result)
{
    if (!task->active.exchange(false))
        return;
    if (images_.value(task->id) == task)
        images_.remove(task->id);
    if (auto reply = uploads_.take(task->id)) {
        reply->disconnect(this);
        if (!reply->isFinished())
            reply->abort();
        reply->deleteLater();
    }
    task->done({result});
    task->bytes.clear();
}

void MultipartServices::imageCancel(const std::shared_ptr<ImageTask> &task)
{
    imageFinish(task, responseError(QNetworkReply::OperationCanceledError, "Image upload canceled"));
}

void MultipartServices::imageApi(
    const std::shared_ptr<ImageTask> &task,
    const QString &stage,
    const QJsonObject &payload,
    std::function<void(QJsonObject)> next)
{
    if (!task->active.load())
        return;
    const auto path = stage == "token" ? QStringLiteral("/api/nos/token/alloc")
                                       : QStringLiteral("/api/upload/event/img/v1");
    auto encrypted = crypto::eapi(
        path, QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
    if (!encrypted) {
        imageFinish(task, responseError(QNetworkReply::ProtocolFailure, encrypted.error()));
        return;
    }
    auto form = "params=" + QUrl::toPercentEncoding(*encrypted);
    auto sent = network_.send(
        "POST",
        QUrl("https://music.163.com/eapi" + path),
        form,
        {{"Content-Type", "application/x-www-form-urlencoded"}, {"Cookie", task->cookie}});
    if (!sent) {
        imageFinish(task, responseError(QNetworkReply::UnknownNetworkError, sent.error()));
        return;
    }
    auto reply = *sent;
    uploads_[task->id] = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, task, stage, next] {
        if (!task->active.load())
            return;
        auto envelope = response(reply);
        auto data = QJsonDocument::fromJson(
                        QByteArray::fromBase64(envelope["bodyBase64"].toString().toLatin1()))
                        .object();
        bool ok = reply->error() == QNetworkReply::NoError && data["code"].toInt() == 200;
        if (uploads_.value(task->id) == reply)
            uploads_.remove(task->id);
        reply->deleteLater();
        if (!ok) {
            imageFinish(task, envelope);
            return;
        }
        if (stage == "register") {
            auto pic = data["picInfo"].toObject();
            if (pic.isEmpty() || (!pic.contains("originId") && !pic.contains("originIdStr"))) {
                envelope["status"] = 502;
                envelope["error"] = int(QNetworkReply::ProtocolFailure);
                envelope["errorString"] = "Image registration returned no picture information";
            }
            imageFinish(task, envelope);
        } else
            next(data);
    });
}

void MultipartServices::imageLocate(const std::shared_ptr<ImageTask> &task)
{
    if (!task->active.load())
        return;
    auto sent
        = network_
              .send("GET", QUrl("https://wanproxy.127.net/lbs?version=1.0&bucketname=cloudmusic"));
    if (!sent) {
        imageFinish(task, responseError(QNetworkReply::UnknownNetworkError, sent.error()));
        return;
    }
    auto reply = *sent;
    uploads_[task->id] = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, task] {
        if (!task->active.load())
            return;
        auto envelope = response(reply);
        auto data = QJsonDocument::fromJson(
                        QByteArray::fromBase64(envelope["bodyBase64"].toString().toLatin1()))
                        .object();
        auto host = data["upload"].toArray().at(0).toString();
        bool ok = reply->error() == QNetworkReply::NoError && !host.isEmpty();
        if (uploads_.value(task->id) == reply)
            uploads_.remove(task->id);
        reply->deleteLater();
        if (!ok) {
            envelope["errorString"] = "Image upload host discovery failed";
            if (!envelope["error"].toInt())
                envelope["error"] = int(QNetworkReply::ProtocolFailure);
            imageFinish(task, envelope);
            return;
        }
        imageBytes(task, host);
    });
}

void MultipartServices::imageBytes(const std::shared_ptr<ImageTask> &task, const QString &uploadHost)
{
    if (!task->active.load())
        return;
    auto host = uploadHost;
    if (!host.startsWith("http://") && !host.startsWith("https://"))
        host = "http://" + host;
    while (host.endsWith('/'))
        host.chop(1);
    auto url = QUrl::fromEncoded(
        host.toUtf8() + "/" + QUrl::toPercentEncoding(task->allocation["bucket"].toString()) + "/"
        + QUrl::toPercentEncoding(task->allocation["objectKey"].toString()));
    QUrlQuery query;
    query.addQueryItem("version", "1.0");
    query.addQueryItem("offset", "0");
    query.addQueryItem("complete", "true");
    url.setQuery(query);
    auto sent = network_.send(
        "POST",
        url,
        task->bytes,
        {{"x-nos-token", task->allocation["token"].toString().toUtf8()},
         {"Content-Type", ("image/" + task->format).toLatin1()}});
    if (!sent) {
        imageFinish(task, responseError(QNetworkReply::UnknownNetworkError, sent.error()));
        return;
    }
    auto reply = *sent;
    uploads_[task->id] = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, task] {
        if (!task->active.load())
            return;
        auto envelope = response(reply);
        bool ok = reply->error() == QNetworkReply::NoError && envelope["status"].toInt() / 100 == 2;
        const auto acknowledgement = QJsonDocument::fromJson(
                                         QByteArray::fromBase64(
                                             envelope["bodyBase64"].toString().toLatin1()))
                                         .object();
        const auto errorCode = acknowledgement.value("errCode");
        const bool hasError = errorCode.isString()   ? !errorCode.toString().isEmpty()
                              : errorCode.isDouble() ? errorCode.toDouble() != 0
                                                     : errorCode.toBool();
        if (ok
            && (!acknowledgement.contains("offset") || hasError
                || acknowledgement["offset"].toVariant().toLongLong() != task->bytes.size())) {
            ok = false;
            envelope["status"] = 502;
            envelope["error"] = int(QNetworkReply::ProtocolFailure);
            envelope["errorString"] = "Image upload acknowledged an incomplete file";
        }
        if (uploads_.value(task->id) == reply)
            uploads_.remove(task->id);
        reply->deleteLater();
        if (!ok) {
            imageFinish(task, envelope);
            return;
        }
        imageApi(
            task,
            "register",
            {{"imgid", task->allocation["docId"].toVariant().toString()}, {"format", task->format}},
            [](QJsonObject) {});
    });
}

MultipartServices::MultipartServices(Bridge &bridge, NativeNetwork &network, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , network_(network)
{
    io_.setMaxThreadCount(1);
    bridge_.addHandler("network.upload", [this](const QJsonArray &args, Bridge::Reply done) {
        auto spec = args.at(0).toObject();
        QMap<QString, QString> parameters;
        for (auto item : QUrlQuery(QUrl(spec["url"].toString())).queryItems(QUrl::FullyDecoded))
            parameters[item.first] = item.second;
        auto body = QByteArray::fromBase64(spec["bodyBase64"].toString().toLatin1());
        for (auto pair : body.split('&')) {
            auto at = pair.indexOf('=');
            if (at < 0)
                continue;
            auto key = pair.left(at), value = pair.mid(at + 1);
            key.replace('+', ' ');
            value.replace('+', ' ');
            parameters[QUrl::fromPercentEncoding(key)] = QUrl::fromPercentEncoding(value);
        }
        auto destination = QUrl(parameters.value("url"));
        if ((destination.scheme() != "https" && destination.scheme() != "http")
            || destination.host() != "music.163.com" || destination.path() != "/upload/event/img") {
            done(
                Bridge::failure(
                    "unsupported_image_endpoint",
                    "The native image adapter supports the supplied "
                    "NetEase event-image endpoint"));
            return;
        }
        auto task = std::make_shared<ImageTask>();
        task->id = spec["id"].toString();
        if (task->id.isEmpty())
            task->id = QUuid::createUuid().toString(QUuid::Id128);
        task->path = parameters.value("path");
        task->cookie = parameters.value("cookie").toUtf8();
        task->account = account();
        task->done = std::move(done);
        if (auto previous = images_.value(task->id))
            imageCancel(previous);
        images_[task->id] = task;
        io_.start([this, task] {
            auto source = readImage(task->path);
            QMetaObject::invokeMethod(this, [this, task, source = std::move(source)]() mutable {
                if (!task->active.load())
                    return;
                if (!source) {
                    imageFinish(
                        task, responseError(QNetworkReply::ContentNotFoundError, source.error()));
                    return;
                }
                task->bytes = std::move(source->bytes);
                task->format = std::move(source->format);
                QFileInfo file(task->path);
                QJsonObject token{
                    {"filename", file.fileName()},
                    {"local", "false"},
                    {"nos_product", 0},
                    {"fileSize", qint64(task->bytes.size())},
                    {"md5", source->md5},
                    {"ext", "." + file.suffix()},
                    {"type", "image"}};
                imageApi(task, "token", token, [this, task](QJsonObject result) {
                    task->allocation = result["result"].toObject();
                    if (task->allocation["bucket"].toString().isEmpty()
                        || task->allocation["objectKey"].toString().isEmpty()
                        || task->allocation["token"].toString().isEmpty()
                        || task->allocation["docId"].toVariant().toString().isEmpty()) {
                        imageFinish(
                            task,
                            {{"status", 502},
                             {"error", int(QNetworkReply::ProtocolFailure)},
                             {"errorString", "Incomplete image upload allocation"}});
                        return;
                    }
                    imageLocate(task);
                });
            });
        });
    });
    connect(&bridge_, &Bridge::invoked, this, [this](const QString &method, const QJsonArray &args) {
        if (method == "network.cancel") {
            auto id = stringArg(args, 0);
            if (auto task = images_.value(id))
                imageCancel(task);
            else if (auto reply = uploads_.value(id))
                reply->abort();
        }
    });
    connect(&network_, &NativeNetwork::sessionChanged, this, [this] {
        auto current = account();
        auto tasks = images_.values();
        for (auto task : tasks)
            if (task->account != current)
                imageCancel(task);
    });
}

MultipartServices::~MultipartServices()
{
    io_.clear();
    io_.waitForDone();
    for (auto task : images_)
        task->active.store(false);
    images_.clear();
    auto replies = uploads_.values();
    uploads_.clear();
    for (auto reply : replies)
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
}

}
