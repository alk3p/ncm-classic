#pragma once

#include "app/RuntimePaths.h"
#include "services/persistence/SettingsStore.h"

#include <QJsonObject>
#include <QObject>

namespace ncm {

class Bridge;
class Skin;
class WebHost;

class AppCommands final : public QObject
{
    Q_OBJECT

public:
    AppCommands(Bridge &, const RuntimePaths &, WebHost &, Skin &, QObject *parent = nullptr);

    QJsonObject proxyConfig() const;

signals:
    void proxyChanged(const QJsonObject &config);

private:
    void registerSettings();
    void registerLifetime();
    void registerSkins();
    void registerUpdates();

    Bridge &bridge_;
    RuntimePaths paths_;
    WebHost &web_;
    Skin &skin_;
    SettingsStore settings_;
};

}
