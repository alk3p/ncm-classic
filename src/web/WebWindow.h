#pragma once

#include <memory>

#include <QJsonArray>
#include <QJsonValue>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <saucer/webview.hpp>

namespace ncm {

class WebHost;

class WebWindow final : public QObject
{
public:
    WebWindow(WebHost &, QString name, std::shared_ptr<saucer::window>, saucer::webview);
    ~WebWindow() override;

    QString name() const { return name_; }

    bool isVisible() const;
    void setVisible(bool);
    void show() { setVisible(true); }
    void hide() { setVisible(false); }
    void activate();
    bool isMinimized() const;
    bool isMaximized() const;
    bool isFullScreen() const;
    void showMinimized();
    void showMaximized();
    void showNormal();
    void setFullScreen(bool);
    void setResizable(bool);
    void setTitle(const QString &);
    bool setIcon(const QString &path);
    WId nativeId() const;

    QSize size() const;
    QRect geometry() const;
    void resize(const QSize &);
    void move(const QPoint &);
    void setSizeLimits(const QSize &minimum, const QSize &maximum);
    void startMove();
    void startResize(Qt::Edges);

    QUrl url() const;
    bool showsAppPage() const;
    void load(const QUrl &);
    saucer::webview &view() { return view_; }

    void deliverEvent(const QString &target, const QString &name, const QJsonArray &args);

private:
    void connectWindow();
    void connectView();
    saucer::policy handleNavigation(const saucer::navigation &);
    void handleFrameNavigation(const QUrl &, bool redirect);
    saucer::status handleMessage(std::string_view message);
    void post(const QJsonObject &message);
    void publish(const QString &name, const QJsonArray &args = {});
    void publishSizeStatus(const QString &state = {});

    WebHost &host_;
    QString name_;
    std::shared_ptr<saucer::window> window_;
    saucer::webview view_;
    QTimer resizeEnd_;
    saucer::window::decoration frame_;
};

QString jsLiteral(const QJsonValue &value);

}
