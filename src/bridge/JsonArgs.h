#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>

namespace ncm {

inline QJsonObject jsonObject(const QJsonValue &value)
{
    return value.isObject() ? value.toObject()
                            : QJsonDocument::fromJson(value.toString().toUtf8()).object();
}

inline int intField(const QJsonObject &object, const QString &key, int fallback = 0)
{
    bool ok = false;
    const int value = object[key].toVariant().toInt(&ok);
    return ok ? value : fallback;
}

inline QJsonObject jsonRect(const QRect &rect)
{
    return {{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}};
}

}
