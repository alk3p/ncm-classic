#include "GlobalHotkeys.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QHash>

#include <windows.h>

namespace ncm {

namespace {

bool registerHotkey(int id, const QList<int> &keys)
{
    UINT modifiers = MOD_NOREPEAT, key = 0;
    for (const auto code : keys) {
        switch (code) {
        case VK_SHIFT:
            modifiers |= MOD_SHIFT;
            break;
        case VK_CONTROL:
            modifiers |= MOD_CONTROL;
            break;
        case VK_MENU:
            modifiers |= MOD_ALT;
            break;
        case VK_LWIN:
            modifiers |= MOD_WIN;
            break;
        default:
            key = UINT(code);
        }
    }
    const bool mediaKey = key >= VK_MEDIA_NEXT_TRACK && key <= VK_MEDIA_PLAY_PAUSE;
    return key && (modifiers != MOD_NOREPEAT || mediaKey)
           && RegisterHotKey(nullptr, id, modifiers, key);
}

}

struct GlobalHotkeys::Platform final : QAbstractNativeEventFilter
{
    explicit Platform(GlobalHotkeys &owner)
        : owner(owner)
    {
        QCoreApplication::instance()->installNativeEventFilter(this);
    }

    ~Platform() override
    {
        for (const auto id : std::as_const(ids))
            UnregisterHotKey(nullptr, id);
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }

    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override
    {
        const auto *msg = static_cast<const MSG *>(message);
        if (msg->message != WM_HOTKEY || msg->hwnd)
            return false;
        const auto name = ids.key(int(msg->wParam));
        if (!name.isEmpty() && owner.activated)
            owner.activated(name);
        return true;
    }

    GlobalHotkeys &owner;
    QHash<QString, int> ids;
    int nextId = 1;
};

GlobalHotkeys::GlobalHotkeys()
    : platform_(std::make_unique<Platform>(*this))
{}

GlobalHotkeys::~GlobalHotkeys() = default;

void GlobalHotkeys::add(const QString &name, const QList<int> &keys)
{
    remove(name);
    const auto id = platform_->nextId++;
    const bool ok = registerHotkey(id, keys);
    if (ok)
        platform_->ids.insert(name, id);
    if (registered)
        registered(name, ok);
}

void GlobalHotkeys::remove(const QString &name)
{
    if (const auto id = platform_->ids.take(name))
        UnregisterHotKey(nullptr, id);
}

}
