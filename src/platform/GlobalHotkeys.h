#pragma once

#include <functional>
#include <memory>

#include <QList>
#include <QString>

namespace ncm {

class GlobalHotkeys final
{
public:
    GlobalHotkeys();
    GlobalHotkeys(const GlobalHotkeys &) = delete;
    GlobalHotkeys &operator=(const GlobalHotkeys &) = delete;
    ~GlobalHotkeys();

    void add(const QString &name, const QList<int> &keys);
    void remove(const QString &name);

    std::function<void(const QString &name, bool ok)> registered;
    std::function<void(const QString &name)> activated;

private:
    struct Platform;
    std::unique_ptr<Platform> platform_;
};

}
