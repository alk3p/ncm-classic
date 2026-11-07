#pragma once

#include <expected>
#include <functional>
#include <optional>

#include <QJsonObject>
#include <QList>
#include <QNetworkCookie>
#include <QObject>

namespace QKeychain {
class Job;
}

namespace ncm {

class CookieVault final : public QObject
{
public:
    struct Contents
    {
        QList<QNetworkCookie> cookies;
        std::optional<QJsonObject> importedAccount;
    };
    using Saved = std::function<void(std::expected<void, QString>)>;

    explicit CookieVault(QObject *parent = nullptr);
    ~CookieVault() override;

    std::expected<Contents, QString> load();
    void save(const Contents &, Saved done = {});
    void erase();

private:
    void track(QKeychain::Job *, Saved done);

    int writesInFlight_ = 0;
};

}
