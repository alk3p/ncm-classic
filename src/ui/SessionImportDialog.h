#pragma once

#include <expected>
#include <functional>

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

namespace ncm {

class SessionImportDialog final : public QDialog
{
public:
    using Result = std::expected<void, QString>;
    using Importer
        = std::function<void(QString cookie, QObject *attempt, std::function<void(Result)> done)>;

    explicit SessionImportDialog(Importer, QWidget *parent = nullptr);

private:
    void submit();
    void finished(Result);

    Importer import_;
    QLineEdit *input_;
    QLabel *status_;
    QPushButton *submit_;
};

}
