#include "NativeNetwork.h"

#include "app/LegacyIdentity.h"
#include "app/RuntimePaths.h"
#include "bridge/Bridge.h"
#include "services/crypto/Crypto.h"

#include <algorithm>
#include <utility>

#include <QCryptographicHash>
#include <QDebug>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QSet>
#include <QSslConfiguration>
#include <QSslError>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

namespace ncm {

namespace {

constexpr auto KeepsCookiesOut = QNetworkRequest::Attribute(QNetworkRequest::User + 17);
constexpr qsizetype maxRequestBody = 32 * 1024 * 1024;
constexpr qint64 maxResponseBody = 64 * 1024 * 1024;
const QUrl sessionAccountUrl("https://music.163.com/weapi/w/nuser/account/get");

bool isMusicHost(const QString &host)
{
    return host == "music.163.com" || host.endsWith(".music.163.com");
}

bool isMusicCookie(const QNetworkCookie &cookie)
{
    auto domain = cookie.domain().toLower();
    if (domain.startsWith('.'))
        domain.remove(0, 1);
    return isMusicHost(domain) || domain == "163.com";
}

bool isSessionCookie(const QByteArray &name)
{
    return name == "MUSIC_U" || name == "MUSIC_A" || name == "__csrf";
}

QUrl secureMusicUrl(QUrl url)
{
    if (url.scheme() == "http" && url.port(80) == 80) {
        url.setScheme("https");
        url.setPort(-1);
    }
    return url;
}

QByteArray authHash(const QList<QNetworkCookie> &cookies)
{
    QList<QByteArray> values;
    for (const auto &c : cookies)
        if (c.name() == "MUSIC_U" || c.name() == "MUSIC_A")
            values.append(
                c.name() + '\0' + c.domain().toUtf8() + '\0' + c.path().toUtf8() + '=' + c.value());
    std::ranges::sort(values);
    return QCryptographicHash::hash(values.join(), QCryptographicHash::Sha256);
}

bool isCookieValue(const QString &value)
{
    return std::ranges::all_of(value, [](QChar ch) {
        const auto c = ch.unicode();
        return c == 0x21 || (c >= 0x23 && c <= 0x2b) || (c >= 0x2d && c <= 0x3a)
               || (c >= 0x3c && c <= 0x5b) || (c >= 0x5d && c <= 0x7e);
    });
}

QList<QNetworkCookie> setCookieHeaders(const QNetworkReply *reply)
{
    QList<QNetworkCookie> cookies;
    for (const auto &[name, value] : reply->rawHeaderPairs())
        if (name.toLower() == "set-cookie")
            cookies.append(QNetworkCookie::parseCookies(value));
    return cookies;
}

}

bool SessionCookies::setCookiesFromUrl(const QList<QNetworkCookie> &cookies, const QUrl &url)
{
    const auto before = allCookies();
    const bool ok = QNetworkCookieJar::setCookiesFromUrl(cookies, url);
    if (!changed)
        return ok;
    const auto after = allCookies();
    for (const auto &old : before)
        if (std::ranges::none_of(after, [&](const auto &c) { return c.hasSameIdentifier(old); }))
            changed(old, true);
    for (const auto &c : after)
        if (std::ranges::none_of(before, [&](const auto &old) {
                return old.toRawForm() == c.toRawForm();
            }))
            changed(c, false);
    return ok;
}

void SessionCookies::importCookie(const QNetworkCookie &cookie, bool removed)
{
    if (removed)
        deleteCookie(cookie);
    else
        insertCookie(cookie);
}

NativeNetwork::NativeNetwork(Bridge &bridge, const RuntimePaths &paths, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , manager_(this)
    , jar_(new SessionCookies(this))
{
    manager_.setCookieJar(jar_);
    manager_.setTransferTimeout(60000);
    if (customProxy_.start(paths.settingsFile)) {
        manager_.setProxy(customProxy_.proxy());
        connect(&customProxy_, &CustomProxy::stopped, this, [this] {
            manager_.setProxy(proxy_);
            manager_.clearConnectionCache();
        });
        connect(
            &manager_,
            &QNetworkAccessManager::sslErrors,
            this,
            [this](QNetworkReply *reply, const QList<QSslError> &errors) {
                if (!isMusicHost(reply->url().host()))
                    return;
                if (customProxy_.isAccepted(reply->sslConfiguration().peerCertificate()))
                    reply->ignoreSslErrors(errors);
            });
    }
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(0);
    connect(&saveTimer_, &QTimer::timeout, this, &NativeNetwork::persist);
    loadCookies();
    jar_->changed = [this](const QNetworkCookie &cookie, bool removed) {
        cookieChanged(cookie, removed);
    };
    connect(&manager_, &QNetworkAccessManager::finished, this, [this](QNetworkReply *reply) {
        if (reply->request().attribute(KeepsCookiesOut).toBool())
            return;
        auto cookies = setCookieHeaders(reply);
        if (!importedAccount_)
            cookies.removeIf([](const QNetworkCookie &c) { return c.name() == "MUSIC_U"; });
        if (!cookies.isEmpty())
            jar_->setCookiesFromUrl(cookies, reply->url());
    });
    registerCookieHandlers();
    registerRequestHandlers();
}

NativeNetwork::~NativeNetwork()
{
    if (saveTimer_.isActive()) {
        saveTimer_.stop();
        persist();
    }
    jar_->changed = {};
    for (auto *manager : findChildren<QNetworkAccessManager *>())
        disconnect(manager, nullptr, this, nullptr);
    for (auto *reply : findChildren<QNetworkReply *>()) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
    }
    requests_.clear();
}

void NativeNetwork::setProxy(const QJsonObject &input)
{
    auto config = input;
    if (input.value("default-key").isString()) {
        const auto parsed = QJsonDocument::fromJson(input.value("default-key").toString().toUtf8());
        if (parsed.isObject())
            config = parsed.object();
    }
    const auto type = config.value("Type").toString(config.value("proxytype").toString());
    const bool useSystem = type == "ie" || type == "system";
    QNetworkProxyFactory::setUseSystemConfiguration(useSystem);
    QNetworkProxy proxy;
    if (useSystem) {
        proxy.setType(QNetworkProxy::DefaultProxy);
    } else if (type.isEmpty() || type == "none") {
        proxy.setType(QNetworkProxy::NoProxy);
    } else {
        const auto values = config.value(type).isObject() ? config.value(type).toObject() : config;
        const auto port = values.value("Port").toVariant().toUInt();
        proxy.setType(
            type.startsWith("socks") ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy);
        proxy.setHostName(values.value("Host").toString(values.value("host").toString()));
        proxy.setPort(port ? port : values.value("port").toVariant().toUInt());
        proxy.setUser(values.value("UserName").toString(values.value("username").toString()));
        proxy.setPassword(values.value("Password").toString(values.value("password").toString()));
    }
    proxy_ = proxy;
    if (!customProxy_.isActive())
        manager_.setProxy(proxy);
    if (!useSystem)
        QNetworkProxy::setApplicationProxy(proxy);
}

std::expected<QNetworkReply *, QString> NativeNetwork::send(
    const QByteArray &method, const QUrl &url, const QByteArray &body, const Headers &headers)
{
    if (QThread::currentThread() != thread())
        return std::unexpected(
            QStringLiteral("NativeNetwork::send must run on its QObject owner thread"));
    return manager_.sendCustomRequest(prepare(url, headers), method, body);
}

QByteArray NativeNetwork::readBody(QNetworkReply *reply)
{
    if (!reply->isOpen() || reply->bytesAvailable() > maxResponseBody)
        return {};
    return reply->readAll();
}

QNetworkRequest NativeNetwork::prepare(const QUrl &url, const Headers &headers) const
{
    const bool imported = importedAccount_ && isMusicHost(url.host());
    auto target = imported ? secureMusicUrl(url) : url;
    if (imported) {
        QUrlQuery query(target);
        if (query.hasQueryItem("anonymousToken")) {
            query.removeAllQueryItems("anonymousToken");
            target.setQuery(query);
        }
    }
    QNetworkRequest request(target);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60000);
    request.setDecompressedSafetyCheckThreshold(maxResponseBody);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setRawHeader("User-Agent", "Mozilla/5.0");
    std::optional<QByteArray> explicitCookie;
    for (const auto &[name, value] : headers) {
        request.setRawHeader(name, value);
        if (name.toLower() == "cookie")
            explicitCookie = value;
    }
    if (!explicitCookie)
        return request;

    QList<QByteArray> parts;
    for (const auto &part : explicitCookie->split(';')) {
        const auto cookie = part.trimmed();
        if (!cookie.isEmpty()
            && !(imported && isSessionCookie(cookie.split('=').value(0).trimmed())))
            parts.append(cookie);
    }
    if (imported)
        request.setAttribute(
            QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    QSet<QByteArray> names;
    for (const auto &part : parts)
        names.insert(part.split('=').value(0));
    for (const auto &c : jar_->cookiesForUrl(target))
        if (!names.contains(c.name()))
            parts.append(c.name() + "=" + c.value());
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setRawHeader("Cookie", parts.join("; "));
    return request;
}

void NativeNetwork::request(
    const QString &id,
    const QByteArray &method,
    const QUrl &url,
    const QByteArray &body,
    const Headers &headers,
    std::function<void(QJsonArray)> done)
{
    const auto sent = send(method, url, body, headers);
    if (!sent) {
        done(Bridge::failure("network_send_failed", sent.error()));
        return;
    }
    auto *reply = *sent;
    if (!id.isEmpty())
        requests_.insert(id, reply);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > maxResponseBody)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, done, id] {
        if (requests_.value(id) == reply)
            requests_.remove(id);
        const auto body = readBody(reply);
        QJsonArray headers;
        for (const auto &[name, value] : reply->rawHeaderPairs())
            if (name.toLower() != "set-cookie" && name.toLower() != "set-cookie2")
                headers.append(QJsonArray{QString::fromLatin1(name), QString::fromLatin1(value)});
        done({QJsonObject{
            {"status", reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()},
            {"headers", headers},
            {"bodyBase64", QString::fromLatin1(body.toBase64())},
            {"error", int(reply->error())},
            {"errorString", reply->errorString()},
            {"finalUrl", reply->url().toString()},
            {"statusText",
             reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString()}}});
        reply->deleteLater();
    });
}

void NativeNetwork::registerRequestHandlers()
{
    bridge_.addHandler("network.request", [this](const QJsonArray &a, Bridge::Reply done) {
        const auto spec = a.at(0).toObject();
        const QUrl url(spec.value("url").toString());
        const auto body = QByteArray::fromBase64(spec.value("bodyBase64").toString().toLatin1());
        if ((url.scheme() != "http" && url.scheme() != "https") || body.size() > maxRequestBody) {
            done(
                Bridge::failure("invalid_request", "Transport requires HTTP(S), body at most 32 MiB"));
            return;
        }
        Headers headers;
        for (const auto header : spec.value("headers").toArray()) {
            const auto pair = header.toArray();
            const auto name = pair.at(0).toString().toLatin1();
            const auto value = pair.at(1).toString().toLatin1();
            if (name.compare("Accept-Encoding", Qt::CaseInsensitive) != 0
                || value.trimmed().compare("identity", Qt::CaseInsensitive) == 0)
                headers.append({name, value});
        }
        request(
            spec.value("id").toString(),
            spec.value("method").toString("GET").toLatin1(),
            url,
            body,
            headers,
            std::move(done));
    });
    bridge_.addHandler("network.cancel", [this](const QJsonArray &a, Bridge::Reply done) {
        if (auto reply = requests_.take(stringArg(a, 0)))
            reply->abort();
        done({});
    });
    bridge_.addHandler("app.testProxy", [this](const QJsonArray &a, Bridge::Reply done) {
        const auto type = stringArg(a, 1);
        const QUrl address("http://" + stringArg(a, 2));
        QNetworkProxy proxy;
        if (type == "none" || type.isEmpty()) {
            proxy.setType(QNetworkProxy::NoProxy);
        } else if (type == "ie") {
            proxy.setType(QNetworkProxy::DefaultProxy);
        } else {
            const bool socks = type.startsWith("socks");
            proxy.setType(socks ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy);
            proxy.setHostName(address.host());
            proxy.setPort(address.port(socks ? 1080 : 80));
            proxy.setUser(stringArg(a, 3));
            proxy.setPassword(stringArg(a, 4));
        }
        auto *manager = new QNetworkAccessManager(this);
        manager->setProxy(proxy);
        QNetworkRequest probe(QUrl(stringArg(a, 5)));
        probe.setTransferTimeout(10000);
        probe.setRawHeader("User-Agent", "Mozilla/5.0");
        probe.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        auto *reply = manager->get(probe);
        connect(reply, &QNetworkReply::finished, this, [this, manager, reply, id = a.at(0), done] {
            const int code = reply->error() == QNetworkReply::NoError ? 0 : -1;
            bridge_.publish("app.ontestproxy", {id, code});
            done({id, code});
            manager->deleteLater();
        });
    });
}

QString NativeNetwork::cookieValue(
    const QUrl &url, const QByteArray &name, const QByteArray &supplied) const
{
    const bool imported = importedAccount_ && isMusicHost(url.host());
    if (!imported || !isSessionCookie(name)) {
        for (const auto &part : supplied.split(';')) {
            const auto item = part.trimmed();
            if (item.startsWith(name + "="))
                return QString::fromUtf8(item.mid(name.size() + 1));
        }
    }
    for (const auto &cookie : jar_->cookiesForUrl(imported ? secureMusicUrl(url) : url))
        if (cookie.name() == name)
            return QString::fromUtf8(cookie.value());
    return {};
}

void NativeNetwork::registerCookieHandlers()
{
    bridge_.addHandler("browser.getCookies", [this](const QJsonArray &a, Bridge::Reply done) {
        QJsonObject values;
        for (const auto &c : jar_->cookiesForUrl(QUrl(stringArg(a, 0))))
            if (!c.isHttpOnly())
                values.insert(QString::fromUtf8(c.name()), QString::fromUtf8(c.value()));
        done({values});
    });
    bridge_.addHandler("browser.setCookie", [this](const QJsonArray &a, Bridge::Reply done) {
        const QUrl url(stringArg(a, 0));
        QNetworkCookie cookie(stringArg(a, 1).toUtf8(), stringArg(a, 2).toUtf8());
        if (isMusicHost(url.host())
            && (cookie.name() == "MUSIC_U"
                || (importedAccount_ && isSessionCookie(cookie.name())))) {
            done({true});
            return;
        }
        cookie.setDomain(url.host());
        cookie.setPath("/");
        done({jar_->setCookiesFromUrl({cookie}, url)});
    });
    bridge_.addHandler("browser.moveCookie", [this](const QJsonArray &a, Bridge::Reply done) {
        const QUrl source(stringArg(a, 0)), destination(stringArg(a, 1));
        const auto name = stringArg(a, 2).toUtf8();
        bool ok = true;
        for (auto c : jar_->cookiesForUrl(source)) {
            if (c.isHttpOnly() || (!name.isEmpty() && c.name() != name))
                continue;
            c.setDomain(destination.host());
            c.setPath("/");
            ok = jar_->setCookiesFromUrl({c}, destination) && ok;
        }
        done({ok});
    });
    bridge_.addHandler("browser.removeCookie", [this](const QJsonArray &a, Bridge::Reply done) {
        const auto host = QUrl(stringArg(a, 0)).host();
        const auto name = stringArg(a, 1).toUtf8();
        for (const auto &c : jar_->snapshot()) {
            auto domain = c.domain();
            if (domain.startsWith('.'))
                domain.remove(0, 1);
            if (c.name() == name && (host == domain || host.endsWith("." + domain)))
                removeCookie(c);
        }
        sessionChangedIfNeeded();
        scheduleSave();
        done({true});
    });
    bridge_.addHandler("app.getimportedsession", [this](const QJsonArray &, Bridge::Reply done) {
        if (bridge_.callingWindow() == "main" && importedAccount_
            && !cookieValue(sessionAccountUrl, "MUSIC_U").isEmpty())
            done({*importedAccount_});
        else
            done({});
    });
}

void NativeNetwork::loadCookies()
{
    auto stored = vault_.load();
    if (!stored)
        qWarning().noquote() << "Cannot read the stored session:" << stored.error();
    const auto session = std::move(stored).value_or(CookieVault::Contents{});
    for (const auto &cookie : session.cookies)
        jar_->importCookie(cookie, false);
    if (session.importedAccount && !cookieValue(sessionAccountUrl, "MUSIC_U").isEmpty())
        importedAccount_ = session.importedAccount;
    authFingerprint_ = authHash(jar_->snapshot());
}

void NativeNetwork::removeCookie(const QNetworkCookie &cookie)
{
    jar_->importCookie(cookie, true);
    if (observer_)
        observer_(cookie, true);
}

void NativeNetwork::cookieChanged(const QNetworkCookie &cookie, bool removed)
{
    if (observer_)
        observer_(cookie, removed);
    sessionChangedIfNeeded();
    scheduleSave();
}

void NativeNetwork::scheduleSave()
{
    saveTimer_.start();
}

void NativeNetwork::persist()
{
    if (cookieValue(sessionAccountUrl, "MUSIC_U").isEmpty())
        vault_.erase();
    else
        vault_.save(contents());
}

CookieVault::Contents NativeNetwork::contents() const
{
    return {.cookies = jar_->snapshot(), .importedAccount = importedAccount_};
}

void NativeNetwork::sessionChangedIfNeeded()
{
    if (importedAccount_ && cookieValue(sessionAccountUrl, "MUSIC_U").isEmpty())
        importedAccount_.reset();
    const auto current = authHash(jar_->snapshot());
    if (current == authFingerprint_)
        return;
    authFingerprint_ = current;
    emit sessionChanged();
}

void NativeNetwork::importSession(QString value, QObject *owner, SessionReply done)
{
    value = value.trimmed();
    if (value.startsWith("MUSIC_U="))
        value.remove(0, 8);
    if (value.endsWith(';'))
        value.chop(1);
    if (value.isEmpty() || value.size() > 8192 || !isCookieValue(value)) {
        done(std::unexpected(tr("请输入单个有效 cookie")));
        return;
    }

    QNetworkCookie cookie("MUSIC_U", value.toLatin1());
    cookie.setDomain(".music.163.com");
    cookie.setPath("/");
    importSession({cookie}, owner, std::move(done));
}

void NativeNetwork::importSession(QList<QNetworkCookie> cookies, QObject *owner, SessionReply done)
{
    cookies.removeIf([](const QNetworkCookie &cookie) {
        return !isMusicCookie(cookie) || (cookie.name() != "MUSIC_U" && cookie.name() != "__csrf")
               || cookie.value().isEmpty() || !isCookieValue(QString::fromLatin1(cookie.value()));
    });
    for (auto &cookie : cookies) {
        cookie.setSecure(true);
        if (cookie.name() == "MUSIC_U")
            cookie.setHttpOnly(true);
    }
    SessionCookies candidate;
    candidate.setCookiesFromUrl(cookies, sessionAccountUrl);
    cookies = candidate.cookiesForUrl(sessionAccountUrl);
    if (std::ranges::none_of(cookies, [](const auto &c) { return c.name() == "MUSIC_U"; })) {
        done(std::unexpected(tr("无法读取登录会话")));
        return;
    }
    QList<QByteArray> parts;
    QString csrf;
    for (const auto &cookie : cookies) {
        parts.append(cookie.toRawForm(QNetworkCookie::NameAndValueOnly));
        if (cookie.name() == "__csrf" && csrf.isEmpty())
            csrf = QString::fromLatin1(cookie.value());
    }
    const auto encrypted = crypto::weapi(
        QJsonDocument(QJsonObject{{"csrf_token", csrf}}).toJson(QJsonDocument::Compact));
    if (!encrypted) {
        done(std::unexpected(tr("无法准备账号验证请求")));
        return;
    }
    const auto body = "params=" + QUrl::toPercentEncoding(encrypted->value("params").toString())
                      + "&encSecKey="
                      + QUrl::toPercentEncoding(encrypted->value("encSecKey").toString());
    auto request = prepare(
        sessionAccountUrl,
        {{"Content-Type", "application/x-www-form-urlencoded"},
         {"User-Agent", legacy::browserUserAgent},
         {"Referer", "https://music.163.com/"}});
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(KeepsCookiesOut, true);
    request.setRawHeader("Cookie", parts.join("; "));
    request.setTransferTimeout(10000);

    auto *reply = manager_.post(request, body);
    connect(owner, &QObject::destroyed, reply, [reply] {
        if (!reply->isFinished())
            reply->abort();
    });
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this,
         reply,
         owner = QPointer(owner),
         cookies = std::move(cookies),
         done = std::move(done)]() mutable {
            reply->deleteLater();
            if (!owner)
                return;
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || status != 200) {
                done(std::unexpected(tr("无法验证此会话（HTTP %1）").arg(status)));
                return;
            }
            const auto result = QJsonDocument::fromJson(readBody(reply)).object();
            const auto account = result.value("account").toObject();
            const auto profile = result.value("profile").toObject();
            if (result.value("code").toVariant().toInt() != 200
                || account.value("id").toVariant().toLongLong() <= 0
                || profile.value("userId").toVariant().toLongLong() <= 0) {
                done(std::unexpected(tr("无法确认账号信息")));
                return;
            }

            for (const auto &c : setCookieHeaders(reply))
                if (c.name() == "__csrf")
                    cookies.append(c);
            const QJsonObject imported{{"account", account}, {"profile", profile}};

            SessionCookies normalized;
            normalized.setCookiesFromUrl(cookies, sessionAccountUrl);
            if (std::ranges::none_of(normalized.cookiesForUrl(sessionAccountUrl), [](const auto &c) {
                    return c.name() == "MUSIC_U";
                })) {
                done(std::unexpected(tr("无法获取会话凭证")));
                return;
            }

            CookieVault::Contents staged{.importedAccount = imported};
            for (const auto &old : jar_->snapshot())
                if (!isMusicCookie(old) || !isSessionCookie(old.name()))
                    staged.cookies.append(old);
            staged.cookies.append(normalized.snapshot());
            vault_.save(
                staged,
                [this, alive = QPointer(&saveTimer_), owner, cookies, imported, done](
                    std::expected<void, QString> saved) {
                    if (!alive)
                        return;
                    if (!saved) {
                        if (owner)
                            done(std::unexpected(tr("无法保存会话凭证")));
                        return;
                    }
                    for (const auto &old : jar_->snapshot())
                        if (isMusicCookie(old) && isSessionCookie(old.name()))
                            removeCookie(old);
                    importedAccount_ = imported;
                    jar_->setCookiesFromUrl(cookies, sessionAccountUrl);
                    bridge_.publishTo("main", "app.onimportedsession", {imported});
                    if (owner)
                        done({});
                });
        });
}

}
