#pragma once

#include <optional>

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace ncm {

class MusicTags
{
public:
    struct Cover
    {
        QByteArray bytes;
        QString mime;
    };

    static std::optional<QJsonObject> read(const QString &path);
    static bool write(const QString &path, const QJsonObject &info, const QString &coverPath = {});
    static std::optional<Cover> cover(const QString &path);
    static std::optional<QString> lyrics(const QString &path);
    static bool supported(const QString &path);
    static bool supportedSuffix(const QString &suffix);
    static QString contentSuffix(const QString &path);
};

}
