#pragma once

#include "app/RuntimePaths.h"

#include <expected>
#include <functional>

#include <QSet>
#include <QUrl>

#include <saucer/scheme.hpp>

namespace ncm {

class ResourceHandler final
{
public:
    explicit ResourceHandler(const RuntimePaths &);

    static bool isAppOrigin(const QUrl &);

    void setKnownFileHandler(std::function<bool(const QString &)> handler)
    {
        knownFile_ = std::move(handler);
    }
    void grantFile(const QString &path);
    bool isKnownFile(const QString &path) const;

    QString resolve(const QUrl &) const;
    QByteArray read(const QUrl &) const;
    void handle(
        const saucer::scheme::request &, const saucer::scheme::executor &, bool pageTrusted) const;

private:
    struct Generated
    {
        QByteArray bytes;
        QByteArray mime;
    };

    static bool isGenerated(const QUrl &);
    std::expected<Generated, saucer::scheme::error> generate(const QUrl &) const;

    RuntimePaths paths_;
    QSet<QString> grants_;
    std::function<bool(const QString &)> knownFile_;
};

}
