#pragma once

#include <functional>
#include <memory>

#include <QNetworkCookie>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <saucer/window.hpp>

namespace saucer {
struct webview;
}

namespace ncm {

class NativeNetwork;

class SignInWindow : public QObject
{
    Q_OBJECT

public:
    static SignInWindow *create(saucer::webview &owner, NativeNetwork &);
    ~SignInWindow() override;
    void activate();

signals:
    void warning(const QString &message);
    void closeRequested();

protected:
    using CookiesReply = std::function<void(QList<QNetworkCookie>)>;

    explicit SignInWindow(std::shared_ptr<saucer::window>, SignInWindow *opener = nullptr);
    static std::shared_ptr<saucer::window> makeWindow(saucer::window &owner, bool popup = false);
    static const QUrl &loginUrl();

    void watchSession(NativeNetwork &);
    virtual void readCookies(CookiesReply) = 0;
    void showUrl(const QUrl &);
    void dismiss();
    bool closing() const { return closing_; }

    std::shared_ptr<saucer::window> window_;

private:
    void checkSession(NativeNetwork &);

    QTimer poll_;
    std::unique_ptr<QObject> attempt_;
    QString warning_;
    bool closing_ = false;
};

}
