#include "SignInWindow.h"

#include <utility>

#include <QDateTime>
#include <QPointer>

#include <saucer/modules/stable/webview2.hpp>
#include <wrl.h>

namespace ncm {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
constexpr auto cookieUrl = L"https://music.163.com/";

QString readString(auto *object, auto getter)
{
    LPWSTR raw = nullptr;
    const auto result = (object->*getter)(&raw);
    const auto text = SUCCEEDED(result) && raw ? QString::fromWCharArray(raw) : QString();
    CoTaskMemFree(raw);
    return text;
}

QList<QNetworkCookie> sessionCookies(ICoreWebView2CookieList *list)
{
    QList<QNetworkCookie> result;
    UINT count = 0;
    if (list)
        list->get_Count(&count);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<ICoreWebView2Cookie> source;
        if (FAILED(list->GetValueAtIndex(index, &source)))
            continue;
        const auto name = readString(source.Get(), &ICoreWebView2Cookie::get_Name);
        if (name != "MUSIC_U" && name != "__csrf")
            continue;
        const auto value = readString(source.Get(), &ICoreWebView2Cookie::get_Value);
        if (value.isEmpty())
            continue;
        QNetworkCookie cookie(name.toLatin1(), value.toLatin1());
        cookie.setDomain(readString(source.Get(), &ICoreWebView2Cookie::get_Domain));
        cookie.setPath(readString(source.Get(), &ICoreWebView2Cookie::get_Path));
        BOOL httpOnly = FALSE, session = TRUE;
        source->get_IsHttpOnly(&httpOnly);
        source->get_IsSession(&session);
        cookie.setHttpOnly(httpOnly);
        double expires = 0;
        if (!session && SUCCEEDED(source->get_Expires(&expires)) && expires > 0)
            cookie.setExpirationDate(QDateTime::fromSecsSinceEpoch(qint64(expires), Qt::UTC));
        result.append(cookie);
    }
    return result;
}

class WebView2SignIn final : public SignInWindow
{
public:
    WebView2SignIn(
        std::shared_ptr<saucer::window> window,
        saucer::window &owner,
        ComPtr<ICoreWebView2Environment10> environment,
        WebView2SignIn *opener = nullptr)
        : SignInWindow(std::move(window), opener)
        , environment_(std::move(environment))
    {
        SetWindowLongPtrW(window_->native().hwnd, GWLP_HWNDPARENT, LONG_PTR(owner.native().hwnd));
        window_->on<saucer::window::event::resize>([this](int, int) { resizeController(); });
    }

    ~WebView2SignIn() override
    {
        window_->off(saucer::window::event::resize);
        if (controller_)
            controller_->Close();
    }

    void start(NativeNetwork &network)
    {
        createController([self = QPointer(this), &network](ICoreWebView2 *view) {
            if (!self || self->closing())
                return;
            ComPtr<ICoreWebView2_2> cookieView;
            const auto target = loginUrl().toString(QUrl::FullyEncoded).toStdWString();
            if (!view || FAILED(self->view_.As(&cookieView))
                || FAILED(cookieView->get_CookieManager(&self->cookies_))
                || FAILED(view->Navigate(target.c_str()))) {
                emit self->warning(tr("无法打开登录页面"));
                self->dismiss();
                return;
            }
            self->watchSession(network);
        });
    }

protected:
    void readCookies(CookiesReply reply) override
    {
        const auto completed = [reply](HRESULT result, ICoreWebView2CookieList *list) {
            reply(SUCCEEDED(result) ? sessionCookies(list) : QList<QNetworkCookie>{});
            return S_OK;
        };
        if (FAILED(cookies_->GetCookies(
                cookieUrl, Callback<ICoreWebView2GetCookiesCompletedHandler>(completed).Get())))
            reply({});
    }

private:
    using Ready = std::function<void(ICoreWebView2 *)>;

    void createController(const Ready &ready)
    {
        ComPtr<ICoreWebView2ControllerOptions> options;
        if (FAILED(environment_->CreateCoreWebView2ControllerOptions(&options))
            || FAILED(options->put_ProfileName(L"SignIn"))
            || FAILED(options->put_IsInPrivateModeEnabled(TRUE))) {
            ready(nullptr);
            return;
        }
        const auto completed = [self = QPointer(this), ready](
                                   HRESULT result, ICoreWebView2Controller *controller) {
            ComPtr<ICoreWebView2> view;
            if (self && !self->closing() && SUCCEEDED(result) && controller
                && SUCCEEDED(controller->get_CoreWebView2(&view))) {
                self->controller_ = controller;
                self->view_ = view;
                self->resizeController();
                self->connectView();
                ready(view.Get());
            } else {
                if (controller)
                    controller->Close();
                ready(nullptr);
            }
            return S_OK;
        };
        const auto handler = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            completed);
        if (FAILED(environment_->CreateCoreWebView2ControllerWithOptions(
                window_->native().hwnd, options.Get(), handler.Get())))
            ready(nullptr);
    }

    void resizeController()
    {
        if (!controller_)
            return;
        RECT bounds{};
        if (GetClientRect(window_->native().hwnd, &bounds))
            controller_->put_Bounds(bounds);
    }

    void connectView()
    {
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(view_->get_Settings(&settings))) {
            settings->put_IsWebMessageEnabled(FALSE);
            settings->put_AreDevToolsEnabled(qEnvironmentVariableIsSet("NCM_WEB_DEVTOOLS"));
        }
        const auto newWindow =
            [this](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) {
                return openPopup(args);
            };
        const auto sourceChanged =
            [this](ICoreWebView2 *view, ICoreWebView2SourceChangedEventArgs *) {
                showUrl(QUrl(readString(view, &ICoreWebView2::get_Source)));
                return S_OK;
            };
        EventRegistrationToken token;
        view_->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(newWindow).Get(), &token);
        view_->add_SourceChanged(
            Callback<ICoreWebView2SourceChangedEventHandler>(sourceChanged).Get(), &token);
        if (parent()) {
            const auto close = [this](ICoreWebView2 *, IUnknown *) {
                QTimer::singleShot(0, this, [this] { dismiss(); });
                return S_OK;
            };
            view_->add_WindowCloseRequested(
                Callback<ICoreWebView2WindowCloseRequestedEventHandler>(close).Get(), &token);
        }
    }

    HRESULT openPopup(ICoreWebView2NewWindowRequestedEventArgs *args)
    {
        args->put_Handled(TRUE);
        BOOL userInitiated = FALSE;
        args->get_IsUserInitiated(&userInitiated);
        if (closing() || !userInitiated)
            return S_OK;
        auto window = makeWindow(*window_, true);
        if (!window)
            return S_OK;
        ComPtr<ICoreWebView2Deferral> deferral;
        if (FAILED(args->GetDeferral(&deferral)))
            return S_OK;
        auto *popup = new WebView2SignIn(std::move(window), *window_, environment_, this);
        ComPtr<ICoreWebView2NewWindowRequestedEventArgs> request(args);
        popup->createController(
            [popup = QPointer(popup), request, deferral](ICoreWebView2 *view) {
                if (popup && view && SUCCEEDED(request->put_NewWindow(view)))
                    popup->activate();
                else if (popup)
                    popup->dismiss();
                deferral->Complete();
            });
        return S_OK;
    }

    ComPtr<ICoreWebView2Environment10> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> view_;
    ComPtr<ICoreWebView2CookieManager> cookies_;
};

}

SignInWindow *SignInWindow::create(saucer::webview &owner, NativeNetwork &network)
{
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Environment10> privateEnvironment;
    auto *view = owner.native().webview;
    if (!view || FAILED(view->get_Environment(&environment))
        || FAILED(environment.As(&privateEnvironment)))
        return nullptr;
    auto window = makeWindow(owner.parent());
    if (!window)
        return nullptr;
    auto *signIn
        = new WebView2SignIn(std::move(window), owner.parent(), std::move(privateEnvironment));
    QTimer::singleShot(0, signIn, [signIn, &network] {
        if (!signIn->closing())
            signIn->start(network);
    });
    return signIn;
}

}
