#include "CookieVault.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDebug>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>

#include <qtkeychain/keychain.h>

namespace ncm {

namespace {

constexpr auto service = NCM_APP_ID;
constexpr auto entry = "session";
constexpr int flushTimeoutMs = 3000;

QByteArray encode(const CookieVault::Contents &contents)
{
    QJsonArray cookies;
    for (const auto &cookie : contents.cookies)
        cookies.append(QString::fromLatin1(cookie.toRawForm()));
    QJsonObject document{{"cookies", cookies}};
    if (contents.importedAccount)
        document["importedAccount"] = *contents.importedAccount;
    return qCompress(QJsonDocument(document).toJson(QJsonDocument::Compact));
}

std::expected<CookieVault::Contents, QString> decode(const QByteArray &data)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(qUncompress(data), &error).object();
    if (error.error != QJsonParseError::NoError)
        return std::unexpected(QStringLiteral("The stored session is unreadable."));

    CookieVault::Contents contents;
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto value : document.value("cookies").toArray())
        for (const auto &cookie : QNetworkCookie::parseCookies(value.toString().toLatin1()))
            if (cookie.isSessionCookie() || cookie.expirationDate() > now)
                contents.cookies.append(cookie);
    if (const auto account = document.value("importedAccount").toObject(); !account.isEmpty())
        contents.importedAccount = account;
    return contents;
}

}

CookieVault::CookieVault(QObject *parent)
    : QObject(parent)
{}

CookieVault::~CookieVault()
{
    const QDeadlineTimer deadline(flushTimeoutMs);
    while (writesInFlight_ && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

std::expected<CookieVault::Contents, QString> CookieVault::load()
{
    QKeychain::ReadPasswordJob job(service);
    job.setAutoDelete(false);
    job.setKey(entry);
    QEventLoop loop;
    connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() == QKeychain::EntryNotFound)
        return Contents{};
    if (job.error() != QKeychain::NoError)
        return std::unexpected(job.errorString());
    return decode(job.binaryData());
}

void CookieVault::save(const Contents &contents, Saved done)
{
    auto *job = new QKeychain::WritePasswordJob(service);
    job->setKey(entry);
    job->setBinaryData(encode(contents));
    track(job, std::move(done));
}

void CookieVault::erase()
{
    auto *job = new QKeychain::DeletePasswordJob(service);
    job->setKey(entry);
    track(job, {});
}

void CookieVault::track(QKeychain::Job *job, Saved done)
{
    ++writesInFlight_;
    connect(job, &QKeychain::Job::finished, this, [this, done = std::move(done)](QKeychain::Job *job) {
        --writesInFlight_;
        const auto error = job->error();
        const bool ok = error == QKeychain::NoError
                        || (error == QKeychain::EntryNotFound
                            && qobject_cast<QKeychain::DeletePasswordJob *>(job));
        if (!ok)
            qWarning().noquote() << "Cannot update the session in the system keychain:"
                                 << job->errorString();
        if (done)
            done(ok ? std::expected<void, QString>{} : std::unexpected(job->errorString()));
    });
    job->start();
}

}
