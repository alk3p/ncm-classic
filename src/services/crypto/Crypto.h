#pragma once

#include <expected>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace ncm::crypto {

std::expected<QString, QString> eapi(const QString &path, const QString &exactJson);
std::expected<QJsonObject, QString> weapi(const QByteArray &exactJson);
std::expected<QJsonValue, QString> pure(const QString &name, const QJsonArray &args);

}
