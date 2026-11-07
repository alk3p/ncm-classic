#pragma once

#include "GlobalHotkeys.h"

namespace ncm {

class Bridge;

class DesktopIntegration final
{
public:
    explicit DesktopIntegration(Bridge &);

private:
    void registerHotkeys();
    void registerAutostart();
    void registerDefaultClient();

    Bridge &bridge_;
    GlobalHotkeys hotkeys_;
};

}
