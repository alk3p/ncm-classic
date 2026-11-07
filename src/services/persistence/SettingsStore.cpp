#include "SettingsStore.h"

#include <QCryptographicHash>
#include <QUuid>

namespace ncm {

SettingsStore::SettingsStore(const Profile &p)
    : settings_(p.paths.settingsFile, QSettings::IniFormat)
    , defaults_(":/ncm/assets/misc/default-settings.ini", QSettings::IniFormat)
{
    settings_.setFallbacksEnabled(false);
    defaults_.setFallbacksEnabled(false);
}

QVariant SettingsStore::get(const QString &g, const QString &k, const QVariant &f) const
{
    const auto key = g + "/" + k;
    return settings_.value(key, defaults_.value(key, f));
}

bool SettingsStore::set(const QString &g, const QString &k, const QVariant &v)
{
    settings_.setValue(g + "/" + k, v);
    settings_.sync();
    return settings_.status() == QSettings::NoError;
}

QString SettingsStore::deviceId()
{
    auto id = get("NativeOS", "device-id").toString();
    if (id.isEmpty()) {
        id = QString::fromLatin1(
            QCryptographicHash::hash(QUuid::createUuid().toRfc4122(), QCryptographicHash::Sha256)
                .toHex()
                .left(52));
        set("NativeOS", "device-id", id);
    }
    return id;
}

}
