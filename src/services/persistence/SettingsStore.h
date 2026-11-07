#pragma once

#include "Profile.h"

#include <QSettings>

namespace ncm {

class SettingsStore
{
public:
    explicit SettingsStore(const Profile &profile);

    QVariant get(const QString &group, const QString &key, const QVariant &fallback = {}) const;
    bool set(const QString &group, const QString &key, const QVariant &value);
    QString deviceId();

private:
    mutable QSettings settings_, defaults_;
};

}
