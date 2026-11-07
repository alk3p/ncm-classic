#pragma once

#include "CookieVault.h"

#include <expected>
#include <functional>
#include <optional>

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace ncm {

class Bridge;
struct RuntimePaths;

class SessionCookies final : public QNetworkCookieJar
{
public:
    using QNetworkCookieJar::QNetworkCookieJar;

    std::function<void(const QNetworkCookie &, bool removed)> changed;

    bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override;
    void importCookie(const QNetworkCookie &, bool removed);
    QList<QNetworkCookie> snapshot() const { return allCookies(); }
};

class NativeNetwork final : public QObject
{
    Q_OBJECT

public:
    using Headers = QList<QPair<QByteArray, QByteArray>>;
    using SessionImportReply = std::function<void(std::expected<void, QString>)>;
    using CookieObserver = std::function<void(const QNetworkCookie &, bool removed)>;

    NativeNetwork(Bridge &, const RuntimePaths &, QObject *parent = nullptr);
    ~NativeNetwork() override;

    void setProxy(const QJsonObject &);

    std::expected<QNetworkReply *, QString> send(
        const QByteArray &method,
        const QUrl &,
        const QByteArray &body = {},
        const Headers &headers = {});
    static QByteArray readBody(QNetworkReply *);

    QList<QNetworkCookie> cookies() const { return jar_->snapshot(); }
    void setCookieObserver(CookieObserver observer) { observer_ = std::move(observer); }
    QString cookieValue(const QUrl &, const QByteArray &name, const QByteArray &supplied = {}) const;

    void importSession(QString value, QObject *owner, SessionImportReply done);

signals:
    void sessionChanged();

private:
    void registerCookieHandlers();
    void registerRequestHandlers();

    void loadCookies();
    void removeCookie(const QNetworkCookie &);
    void cookieChanged(const QNetworkCookie &, bool removed);
    void sessionChangedIfNeeded();
    void scheduleSave();
    void persist();
    CookieVault::Contents contents() const;

    QNetworkRequest prepare(const QUrl &, const Headers &headers) const;
    void request(
        const QString &id,
        const QByteArray &method,
        const QUrl &url,
        const QByteArray &body,
        const Headers &headers,
        std::function<void(QJsonArray)> done);

    Bridge &bridge_;
    QNetworkAccessManager manager_;
    SessionCookies *jar_;
    CookieVault vault_;
    CookieObserver observer_;
    QHash<QString, QPointer<QNetworkReply>> requests_;
    std::optional<QJsonObject> importedAccount_;
    QByteArray authFingerprint_;
    QTimer saveTimer_;
};

}
