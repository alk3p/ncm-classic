#pragma once

#include <memory>

#include <QDialog>
#include <QPointer>

class QLineEdit;
class QMessageBox;
class QPushButton;

namespace ncm {

class NativeNetwork;
class SignInWindow;
class WebHost;

class SessionImportDialog final : public QDialog
{
public:
    SessionImportDialog(WebHost &, NativeNetwork &);
    void activate();

protected:
    void done(int result) override;

private:
    void submit();
    void loginOnline();
    void setPending(bool pending);
    void warn(const QString &message);

    WebHost &web_;
    NativeNetwork &network_;
    QLineEdit *input_;
    QPushButton *submit_;
    QPushButton *online_;
    std::unique_ptr<QObject> attempt_;
    QPointer<SignInWindow> signIn_;
    QPointer<QMessageBox> warning_;
};

}
