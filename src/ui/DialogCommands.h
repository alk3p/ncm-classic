#pragma once

#include <functional>

#include <QJsonArray>
#include <QObject>
#include <QPointer>

class QDialog;
class QFileDialog;

namespace ncm {

class Bridge;
class NativeNetwork;
class SessionImportDialog;
class WebHost;

class DialogCommands final : public QObject
{
public:
    DialogCommands(Bridge &, WebHost &, NativeNetwork &, QObject *parent = nullptr);
    ~DialogCommands() override;

    void openSignIn();

private:
    void open(QDialog *dialog);
    void pickFiles(QFileDialog *dialog, std::function<void(QStringList)> done);
    QJsonArray grantFiles(const QStringList &paths);

    Bridge &bridge_;
    WebHost &web_;
    NativeNetwork &network_;
    QPointer<SessionImportDialog> signIn_;
};

}
