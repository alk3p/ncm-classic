#include "DialogCommands.h"

#include "bridge/Bridge.h"
#include "web/WebHost.h"

#include <QColorDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonObject>
#include <QWindow>

namespace ncm {

namespace {

QString fileDialogFilter(const QString &windows)
{
    const auto parts = windows.split(QChar(0), Qt::SkipEmptyParts);
    if (parts.size() < 2)
        return windows;
    QStringList filters;
    for (qsizetype i = 0; i + 1 < parts.size(); i += 2) {
        auto name = parts[i];
        if (const auto patterns = name.indexOf('('); patterns >= 0)
            name.truncate(patterns);
        filters << name.trimmed() + " (" + parts[i + 1].split(';').join(' ') + ')';
    }
    return filters.join(";;");
}

}

DialogCommands::DialogCommands(
    Bridge &bridge, WebHost &web, SessionImportDialog::Importer importer, QObject *parent)
    : QObject(parent)
    , bridge_(bridge)
    , web_(web)
    , importer_(std::move(importer))
{
    const auto add = [&bridge](const QString &name, Bridge::Handler handler) {
        bridge.addHandler("app." + name, std::move(handler));
    };

    add("selectSystemDir", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto key = stringArg(a, 0), owner = bridge_.callingDocument();
        auto *dialog = new QFileDialog(nullptr, stringArg(a, 1), stringArg(a, 3));
        dialog->setFileMode(QFileDialog::Directory);
        dialog->setOption(QFileDialog::ShowDirsOnly);
        pickFiles(dialog, [this, key, owner, reply](const QStringList &paths) {
            const auto path = paths.value(0);
            bridge_.publishTo(owner, "app.onselectsystemfile", {!path.isEmpty(), key, path});
            reply({!path.isEmpty(), key, path});
        });
    });
    add("openSaveFileDialog", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto key = stringArg(a, 0), owner = bridge_.callingDocument();
        auto *dialog = new QFileDialog(
            nullptr, stringArg(a, 1), stringArg(a, 3), fileDialogFilter(stringArg(a, 2)));
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        pickFiles(dialog, [this, key, owner, reply](const QStringList &paths) {
            const auto path = paths.value(0);
            bridge_.publishTo(owner, "app.onSaveFileDialog", {key, !path.isEmpty(), path});
            reply({path});
        });
    });
    add("selectSystemFileLimitCount", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto key = stringArg(a, 0), owner = bridge_.callingDocument();
        const int limit = numberArg(a, 4, 10000);
        auto *dialog
            = new QFileDialog(nullptr, tr("选择文件"), QString(), fileDialogFilter(stringArg(a, 3)));
        dialog->setFileMode(QFileDialog::ExistingFiles);
        pickFiles(dialog, [this, key, owner, limit, reply](const QStringList &paths) {
            const auto files = grantFiles(paths.mid(0, limit));
            bridge_.publishTo(owner, "app.onSelectFileLimitCount", {key, !files.isEmpty(), files});
            reply({files});
        });
    });
    add("chooseColor", [this](const QJsonArray &a, Bridge::Reply reply) {
        const auto id = stringArg(a, 0), owner = bridge_.callingDocument();
        auto *dialog = new QColorDialog(QColor::fromRgb(stringArg(a, 1).toUInt(nullptr, 16)));
        dialog->setWindowTitle(tr("Choose Color"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::finished, this, [this, dialog, id, owner, reply](int result) {
            const auto color = dialog->selectedColor();
            if (result == QDialog::Accepted)
                bridge_.publishTo(
                    owner,
                    "app.onchooseColorResult",
                    {id, color.red(), color.green(), color.blue()});
            reply({result == QDialog::Accepted});
        });
        open(dialog);
    });
}

void DialogCommands::openSignIn()
{
    if (signIn_) {
        signIn_->raise();
        signIn_->activateWindow();
        return;
    }
    signIn_ = new SessionImportDialog(importer_);
    signIn_->setAttribute(Qt::WA_DeleteOnClose);
    open(signIn_);
}

void DialogCommands::open(QDialog *dialog)
{
    if (auto *owner = web_.window(); owner && owner->nativeId()) {
        dialog->winId();
        if (auto *parent = QWindow::fromWinId(owner->nativeId())) {
            connect(dialog, &QObject::destroyed, parent, &QObject::deleteLater);
            dialog->windowHandle()->setTransientParent(parent);
        }
    }
    dialog->open();
}

void DialogCommands::pickFiles(QFileDialog *dialog, std::function<void(QStringList)> done)
{
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [dialog, done = std::move(done)](int result) {
        done(result == QDialog::Accepted ? dialog->selectedFiles() : QStringList());
    });
    open(dialog);
}

QJsonArray DialogCommands::grantFiles(const QStringList &paths)
{
    QJsonArray files;
    for (const auto &path : paths) {
        web_.resources().grantFile(path);
        const QFileInfo info(path);
        files.append(
            QJsonObject{{"path", path}, {"name", info.fileName()}, {"isDir", info.isDir()}});
    }
    return files;
}

}
