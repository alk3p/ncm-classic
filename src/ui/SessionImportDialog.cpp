#include "SessionImportDialog.h"

#include "services/network/NativeNetwork.h"
#include "web/SignInWindow.h"
#include "web/WebHost.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace ncm {

SessionImportDialog::SessionImportDialog(WebHost &web, NativeNetwork &network)
    : QDialog(
          nullptr,
          Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
              | Qt::WindowCloseButtonHint)
    , web_(web)
    , network_(network)
    , input_(new QLineEdit(this))
    , online_(new QPushButton(tr("在线登录"), this))
{
    setWindowTitle(tr("登录网易云音乐"));
    setMinimumWidth(360);

    auto *prompt = new QLabel(tr("MUSIC_U:"), this);
    input_->setEchoMode(QLineEdit::Password);
    online_->setAutoDefault(false);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    submit_ = buttons->button(QDialogButtonBox::Ok);
    submit_->setDefault(true);

    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(prompt);
    inputRow->addWidget(input_, 1);

    auto *actions = new QHBoxLayout;
    actions->addWidget(online_);
    actions->addStretch();
    actions->addWidget(buttons);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(inputRow);
    layout->addLayout(actions);

    connect(buttons, &QDialogButtonBox::accepted, this, &SessionImportDialog::submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(online_, &QPushButton::clicked, this, &SessionImportDialog::loginOnline);
    input_->setFocus();
}

void SessionImportDialog::activate()
{
    if (warning_) {
        warning_->raise();
        warning_->activateWindow();
    } else if (signIn_) {
        signIn_->activate();
    } else {
        raise();
        activateWindow();
    }
}

void SessionImportDialog::done(int result)
{
    attempt_.reset();
    QDialog::done(result);
}

void SessionImportDialog::submit()
{
    if (attempt_ || signIn_)
        return;
    attempt_ = std::make_unique<QObject>();
    setPending(true);
    network_.importSession(input_->text(), attempt_.get(), [this](auto result) {
        attempt_.reset();
        if (result) {
            accept();
            return;
        }
        setPending(false);
        input_->setFocus();
        input_->selectAll();
        warn(result.error());
    });
}

void SessionImportDialog::loginOnline()
{
    if (attempt_ || signIn_)
        return;
    auto *window = web_.createSignInWindow(network_);
    if (!window) {
        warn(tr("无法打开登录页面"));
        return;
    }
    signIn_ = window;
    connect(window, &SignInWindow::warning, this, &SessionImportDialog::warn);
    connect(window, &SignInWindow::closeRequested, this, &QObject::deleteLater);
    connect(window, &QObject::destroyed, this, [this] {
        if (warning_)
            connect(warning_.data(), &QObject::destroyed, this, &QObject::deleteLater);
        else
            deleteLater();
    });
    hide();
    window->activate();
}

void SessionImportDialog::setPending(bool pending)
{
    input_->setEnabled(!pending);
    submit_->setEnabled(!pending);
    online_->setEnabled(!pending);
}

void SessionImportDialog::warn(const QString &message)
{
    if (warning_) {
        warning_->setText(message);
        return;
    }
    warning_ = new QMessageBox(QMessageBox::Warning, windowTitle(), message, QMessageBox::Ok, this);
    warning_->setAttribute(Qt::WA_DeleteOnClose);
    warning_->setWindowModality(Qt::ApplicationModal);
    warning_->setTextFormat(Qt::PlainText);
    warning_->show();
}

}
