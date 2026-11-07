#include "SessionImportDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace ncm {

SessionImportDialog::SessionImportDialog(Importer import, QWidget *parent)
    : QDialog(parent)
    , import_(std::move(import))
    , input_(new QLineEdit(this))
    , status_(new QLabel(this))
{
    setWindowTitle(tr("导入网易云音乐会话"));
    setMinimumWidth(360);

    auto *prompt = new QLabel(tr("MUSIC_U:"), this);
    input_->setEchoMode(QLineEdit::Password);
    input_->setMaxLength(8192);
    status_->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    submit_ = buttons->button(QDialogButtonBox::Ok);

    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(prompt);
    inputRow->addWidget(input_, 1);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(inputRow);
    layout->addWidget(status_);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &SessionImportDialog::submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    input_->setFocus();
}

void SessionImportDialog::submit()
{
    if (!submit_->isEnabled())
        return;
    input_->setEnabled(false);
    submit_->setEnabled(false);
    status_->clear();
    auto *attempt = new QObject(this);
    connect(this, &QDialog::rejected, attempt, [attempt] { delete attempt; });
    import_(input_->text(), attempt, [this, attempt](Result result) {
        attempt->deleteLater();
        finished(std::move(result));
    });
}

void SessionImportDialog::finished(Result result)
{
    if (result) {
        input_->clear();
        accept();
        return;
    }
    status_->setText(result.error());
    input_->setEnabled(true);
    submit_->setEnabled(true);
    input_->setFocus();
    input_->selectAll();
}

}
