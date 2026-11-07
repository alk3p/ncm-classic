#pragma once

#include "SessionImportDialog.h"

#include <functional>

#include <QJsonArray>
#include <QObject>
#include <QPointer>

class QDialog;
class QFileDialog;

namespace ncm {

class Bridge;
class WebHost;

class DialogCommands final : public QObject
{
public:
    DialogCommands(Bridge &, WebHost &, SessionImportDialog::Importer, QObject *parent = nullptr);

    void openSignIn();

private:
    void open(QDialog *dialog);
    void pickFiles(QFileDialog *dialog, std::function<void(QStringList)> done);
    QJsonArray grantFiles(const QStringList &paths);

    Bridge &bridge_;
    WebHost &web_;
    SessionImportDialog::Importer importer_;
    QPointer<SessionImportDialog> signIn_;
};

}
