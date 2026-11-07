#pragma once

#include "ResourceHandler.h"
#include "WebWindow.h"
#include "app/RuntimePaths.h"

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include <QList>
#include <QNetworkCookie>
#include <QObject>

#include <saucer/app.hpp>

namespace ncm {

class Bridge;

class WebHost final : public QObject
{
public:
    WebHost(Bridge &, const RuntimePaths &);
    ~WebHost() override;

    std::expected<void, QString> start();
    void shutdown();

    Bridge &bridge() { return bridge_; }
    ResourceHandler &resources() { return resources_; }

    WebWindow *createWindow(const QString &name, const QUrl &url, bool visible = true);
    WebWindow *window(const QString &name = {}) const;
    QList<WebWindow *> windows() const;
    void destroyWindow(const QString &name);

    void setCookieSource(std::function<QList<QNetworkCookie>()> source);
    void mirrorCookie(const QNetworkCookie &, bool removed);

private:
    std::unique_ptr<WebWindow> makeWindow(const QString &name, const QUrl &url);
    void attachCookieStore(WebWindow &);

    Bridge &bridge_;
    RuntimePaths paths_;
    ResourceHandler resources_;
    std::optional<saucer::application> app_;
    QString bridgeScript_;
    std::map<QString, std::unique_ptr<WebWindow>> windows_;
    std::function<QList<QNetworkCookie>()> cookieSource_;
    bool cookieStoreAttached_ = false;
};

}
