#include "GlobalHotkeys.h"

#include <utility>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QUuid>

namespace ncm {

namespace {

constexpr auto portalService = "org.freedesktop.portal.Desktop";
constexpr auto portalPath = "/org/freedesktop/portal/desktop";
constexpr auto requestInterface = "org.freedesktop.portal.Request";
constexpr auto sessionInterface = "org.freedesktop.portal.Session";
constexpr auto shortcutsInterface = "org.freedesktop.portal.GlobalShortcuts";

constexpr std::pair<int, const char *> keyNames[] = {
    {8, "BackSpace"},
    {9, "Tab"},
    {13, "Return"},
    {27, "Escape"},
    {32, "space"},
    {33, "Prior"},
    {34, "Next"},
    {35, "End"},
    {36, "Home"},
    {37, "Left"},
    {38, "Up"},
    {39, "Right"},
    {40, "Down"},
    {45, "Insert"},
    {46, "Delete"},
    {176, "XF86AudioNext"},
    {177, "XF86AudioPrev"},
    {178, "XF86AudioStop"},
    {179, "XF86AudioPlay"},
    {186, "semicolon"},
    {187, "equal"},
    {188, "comma"},
    {189, "minus"},
    {190, "period"},
    {191, "slash"},
    {192, "grave"},
    {219, "bracketleft"},
    {220, "backslash"},
    {221, "bracketright"},
    {222, "apostrophe"},
};

QString keyName(int code)
{
    if (code >= 'A' && code <= 'Z')
        return QChar(code).toLower();
    if (code >= '0' && code <= '9')
        return QChar(code);
    if (code >= 96 && code <= 105)
        return "KP_" + QString::number(code - 96);
    if (code >= 112 && code <= 135)
        return 'F' + QString::number(code - 111);
    for (const auto &[known, name] : keyNames)
        if (known == code)
            return name;
    return {};
}

QString trigger(const QList<int> &keys)
{
    QStringList parts;
    QString key;
    for (const auto code : keys) {
        switch (code) {
        case 16:
            parts << "SHIFT";
            break;
        case 17:
            parts << "CTRL";
            break;
        case 18:
            parts << "ALT";
            break;
        case 91:
            parts << "LOGO";
            break;
        default:
            key = keyName(code);
        }
    }
    return key.isEmpty() ? QString() : (parts << key).join('+');
}

struct Shortcut
{
    QString id;
    QVariantMap properties;
};

QDBusArgument &operator<<(QDBusArgument &argument, const Shortcut &shortcut)
{
    argument.beginStructure();
    argument << shortcut.id << shortcut.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Shortcut &shortcut)
{
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.properties;
    argument.endStructure();
    return argument;
}

QString newToken()
{
    return "ncm" + QUuid::createUuid().toString(QUuid::Id128);
}

QString requestPath(const QString &token)
{
    auto sender = QDBusConnection::sessionBus().baseService().mid(1);
    return "/org/freedesktop/portal/desktop/request/" + sender.replace('.', '_') + '/' + token;
}

}

class ShortcutsPortal : public QObject
{
    Q_OBJECT

public:
    explicit ShortcutsPortal(GlobalHotkeys &owner);
    ~ShortcutsPortal() override;

    void add(const QString &name, const QString &trigger);
    void remove(const QString &name);

private slots:
    void handleResponse(uint code, const QVariantMap &results, const QDBusMessage &message);
    void handleActivated(
        const QDBusObjectPath &session,
        const QString &id,
        qulonglong timestamp,
        const QVariantMap &options);

private:
    enum class Step { Idle, Creating, Binding };

    void bind();
    void call(const QString &method, const QVariantList &arguments, const QString &token);
    void finish(bool ok);
    void closeSession();

    GlobalHotkeys &owner_;
    QHash<QString, QString> triggers_;
    QHash<QString, QString> requested_;
    QSet<QString> unanswered_;
    QSet<QString> bound_;
    QString session_;
    QString request_;
    Step step_ = Step::Idle;
    bool stale_ = false;
    QTimer batch_;
};

ShortcutsPortal::ShortcutsPortal(GlobalHotkeys &owner)
    : owner_(owner)
{
    qDBusRegisterMetaType<Shortcut>();
    qDBusRegisterMetaType<QList<Shortcut>>();
    batch_.setSingleShot(true);
    batch_.setInterval(100);
    connect(&batch_, &QTimer::timeout, this, &ShortcutsPortal::bind);

    auto bus = QDBusConnection::sessionBus();
    bus.connect(
        portalService,
        {},
        requestInterface,
        "Response",
        this,
        SLOT(handleResponse(uint, QVariantMap, QDBusMessage)));
    bus.connect(
        portalService,
        portalPath,
        shortcutsInterface,
        "Activated",
        this,
        SLOT(handleActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
}

ShortcutsPortal::~ShortcutsPortal()
{
    closeSession();
}

void ShortcutsPortal::add(const QString &name, const QString &trigger)
{
    triggers_.insert(name, trigger);
    unanswered_.insert(name);
    batch_.start();
}

void ShortcutsPortal::remove(const QString &name)
{
    if (!triggers_.remove(name))
        return;
    unanswered_.remove(name);
    batch_.start();
}

void ShortcutsPortal::bind()
{
    if (step_ != Step::Idle) {
        stale_ = true;
        return;
    }
    closeSession();
    if (triggers_.isEmpty())
        return;
    requested_ = triggers_;
    step_ = Step::Creating;
    const auto token = newToken();
    call(
        "CreateSession",
        {QVariantMap{{"handle_token", token}, {"session_handle_token", token}}},
        token);
}

void ShortcutsPortal::call(const QString &method, const QVariantList &arguments, const QString &token)
{
    request_ = requestPath(token);
    auto message
        = QDBusMessage::createMethodCall(portalService, portalPath, shortcutsInterface, method);
    message.setArguments(arguments);
    auto *watcher
        = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        watcher->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        if (reply.isError()) {
            qWarning() << "Global shortcuts are unavailable:" << reply.error().message();
            finish(false);
            return;
        }
        request_ = reply.value().path();
    });
}

void ShortcutsPortal::handleResponse(
    uint code, const QVariantMap &results, const QDBusMessage &message)
{
    if (request_.isEmpty() || message.path() != request_)
        return;
    request_.clear();
    if (code != 0) {
        finish(false);
        return;
    }

    if (step_ == Step::Creating) {
        const auto handle = results.value("session_handle");
        session_ = handle.metaType() == QMetaType::fromType<QDBusObjectPath>()
                       ? handle.value<QDBusObjectPath>().path()
                       : handle.toString();
        if (session_.isEmpty()) {
            finish(false);
            return;
        }
        QList<Shortcut> shortcuts;
        for (const auto &[name, keys] : requested_.asKeyValueRange())
            shortcuts.append({name, {{"description", name}, {"preferred_trigger", keys}}});
        step_ = Step::Binding;
        const auto token = newToken();
        call(
            "BindShortcuts",
            {QVariant::fromValue(QDBusObjectPath(session_)),
             QVariant::fromValue(shortcuts),
             QString(),
             QVariantMap{{"handle_token", token}}},
            token);
        return;
    }

    for (const auto &shortcut : qdbus_cast<QList<Shortcut>>(results.value("shortcuts")))
        bound_.insert(shortcut.id);
    finish(true);
}

void ShortcutsPortal::handleActivated(
    const QDBusObjectPath &session, const QString &id, qulonglong, const QVariantMap &)
{
    if (session.path() == session_ && bound_.contains(id) && owner_.activated)
        owner_.activated(id);
}

void ShortcutsPortal::finish(bool ok)
{
    step_ = Step::Idle;
    request_.clear();
    for (const auto &name : requested_.keys())
        if (unanswered_.remove(name) && owner_.registered)
            owner_.registered(name, ok && bound_.contains(name));
    requested_.clear();
    if (std::exchange(stale_, false))
        batch_.start();
}

void ShortcutsPortal::closeSession()
{
    bound_.clear();
    if (session_.isEmpty())
        return;
    QDBusConnection::sessionBus().send(
        QDBusMessage::createMethodCall(portalService, session_, sessionInterface, "Close"));
    session_.clear();
}

struct GlobalHotkeys::Platform final : ShortcutsPortal
{
    using ShortcutsPortal::ShortcutsPortal;
};

GlobalHotkeys::GlobalHotkeys()
    : platform_(std::make_unique<Platform>(*this))
{}

GlobalHotkeys::~GlobalHotkeys() = default;

void GlobalHotkeys::add(const QString &name, const QList<int> &keys)
{
    if (const auto shortcut = trigger(keys); !shortcut.isEmpty())
        platform_->add(name, shortcut);
    else if (registered)
        registered(name, false);
}

void GlobalHotkeys::remove(const QString &name)
{
    platform_->remove(name);
}

}

#include "GlobalHotkeys_linux.moc"
