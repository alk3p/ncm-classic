#pragma once

#include <expected>

#include <QJsonObject>
#include <QString>

namespace ncm {

class CueParser
{
public:
    static std::expected<QJsonObject, QString> parse(const QString &path);
};

}
