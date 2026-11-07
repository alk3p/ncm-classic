#include "GlobalHotkeys.h"

namespace ncm {

struct GlobalHotkeys::Platform
{};

GlobalHotkeys::GlobalHotkeys() = default;

GlobalHotkeys::~GlobalHotkeys() = default;

void GlobalHotkeys::add(const QString &name, const QList<int> &)
{
    if (registered)
        registered(name, false);
}

void GlobalHotkeys::remove(const QString &) {}

}
