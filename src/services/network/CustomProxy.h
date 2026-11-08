#pragma once

#include <QNetworkProxy>
#include <QProcess>
#include <QSslCertificate>

namespace ncm {

class CustomProxy final : public QObject
{
    Q_OBJECT

public:
    ~CustomProxy() override;

    bool start(const QString &);
    bool isAccepted(const QSslCertificate &) const;
    bool isActive() const { return proxy_.type() == QNetworkProxy::HttpProxy; }
    QNetworkProxy proxy() const { return proxy_; }

signals:
    void stopped();

private:
    bool useRemote(const QString &);
    void stopProcess();

    QProcess process_;
    QNetworkProxy proxy_;
    QByteArray pinned_;
};

}
