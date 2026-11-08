#include "CustomProxy.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace ncm {

namespace {

const auto loopback = QHostAddress(QHostAddress::LocalHost);
constexpr int startupTimeout = 5000;
constexpr int pollInterval = 50;

quint16 reservePort(QTcpServer &server, quint16 otherPort = 0)
{
    server.setProxy(QNetworkProxy::NoProxy);
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto port = quint16(QRandomGenerator::global()->bounded(49152, 65536));
        if (port != otherPort && server.listen(loopback, port))
            return port;
    }
    return 0;
}

bool isListening(quint16 port)
{
    QTcpSocket socket;
    socket.setProxy(QNetworkProxy::NoProxy);
    socket.connectToHost(loopback, port);
    return socket.waitForConnected(pollInterval);
}

}

CustomProxy::~CustomProxy()
{
    disconnect(&process_, nullptr, this, nullptr);
    stopProcess();
}

bool CustomProxy::start(const QString &settingsFile)
{
    QSettings settings(settingsFile, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    settings.beginGroup("Unblock");
    const auto path = settings.value("path").toString().trimmed();
    const auto url = settings.value("url").toString().trimmed();
    if (path.isEmpty() && url.isEmpty())
        return false;

    const auto configuredHash = settings.value("cert-hash").toByteArray().trimmed();
    if (!configuredHash.isEmpty()) {
        const auto hash = configuredHash.toLower().replace(":", "");
        if (hash.size() != 64 || QByteArray::fromHex(hash).toHex() != hash) {
            qWarning() << "Custom proxy cert-hash must be a SHA-256 certificate fingerprint";
            return false;
        }
        pinned_ = hash;
    }
    if (path.isEmpty())
        return useRemote(url);

    const QFileInfo program(path);
    if (!program.isFile()) {
        qWarning().noquote() << "Custom proxy executable not found" << path;
        return false;
    }

    auto sources = settings.value("source").toStringList().join(',').split(',');
    for (auto &source : sources)
        source = source.trimmed();
    sources.removeAll(QString());
    if (std::ranges::any_of(sources, [](const QString &source) { return source.startsWith('-'); })) {
        qWarning() << "Custom proxy source must contain provider names";
        return false;
    }

    auto environment = QProcessEnvironment::systemEnvironment();
    settings.beginGroup("env");
    for (const auto &name : settings.childKeys())
        environment.insert(name, settings.value(name).toString());

    QTcpServer http, https;
    const auto httpPort = reservePort(http);
    const auto httpsPort = reservePort(https, httpPort);
    if (!httpPort || !httpsPort) {
        qWarning() << "Cannot reserve loopback ports for the custom proxy";
        return false;
    }
    const auto host = loopback.toString();
    QStringList args{
        "--address",
        host,
        "--port",
        QString("%1:%2").arg(httpPort).arg(httpsPort),
        "--endpoint",
        QString("http://%1:%2").arg(host).arg(httpPort),
    };
    if (!sources.isEmpty()) {
        args.append("--match-order");
        args.append(sources);
    }

    process_.setProcessEnvironment(environment);
    process_.setStandardOutputFile(QProcess::nullDevice());
    process_.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    process_.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *arguments) { arguments->flags |= CREATE_NO_WINDOW; });
#endif
    connect(&process_, &QProcess::finished, this, [this](int exitCode) {
        if (!isActive())
            return;
        proxy_ = QNetworkProxy();
        qWarning() << "Custom proxy stopped with exit code" << exitCode;
        emit stopped();
    });

    QElapsedTimer timer;
    timer.start();
    http.close();
    https.close();
    process_.start(program.absoluteFilePath(), args);
    if (process_.waitForStarted(startupTimeout)) {
        while (timer.elapsed() < startupTimeout && process_.state() == QProcess::Running) {
            if (isListening(httpPort) && isListening(httpsPort)) {
                proxy_ = {QNetworkProxy::HttpProxy, host, httpPort};
                return true;
            }
            process_.waitForFinished(pollInterval);
        }
        qWarning() << "Custom proxy failed to open its loopback ports";
    } else {
        qWarning() << "Cannot start the custom proxy" << process_.errorString();
    }
    stopProcess();
    return false;
}

bool CustomProxy::useRemote(const QString &address)
{
    const QUrl url(address, QUrl::StrictMode);
    const int port = url.port(80);
    if (!url.isValid() || url.scheme() != "http" || url.host().isEmpty() || port <= 0
        || (!url.path().isEmpty() && url.path() != "/") || url.hasQuery() || url.hasFragment()) {
        qWarning() << "Custom proxy url must identify an HTTP proxy listener";
        return false;
    }
    proxy_ = {QNetworkProxy::HttpProxy, url.host(), quint16(port), url.userName(), url.password()};
    return true;
}

void CustomProxy::stopProcess()
{
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished();
    }
}

bool CustomProxy::isAccepted(const QSslCertificate &certificate) const
{
    if (!isActive() || certificate.isNull())
        return false;
    const auto hash = certificate.digest(QCryptographicHash::Sha256).toHex();
    if (!pinned_.isEmpty())
        return hash == pinned_;
    return hash == "3f583a730684b7d0c0c4738534546f73226c5f07feacce3d32dd49effebcd644"
           || hash == "3f5b87428175a131f744833c5ea987ae666b9a3786fa0b473fa1695e139d79ef";
}

}
