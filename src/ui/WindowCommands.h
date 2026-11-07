#pragma once

#include <QObject>

namespace ncm {

class Bridge;
class DialogCommands;
class PlayerWindows;
class WebHost;

class WindowCommands final : public QObject
{
public:
    WindowCommands(Bridge &, WebHost &, PlayerWindows &, DialogCommands &, QObject *parent = nullptr);

private:
    void registerWebWindows();
    void registerGeometry();
    void registerClipboard();

    Bridge &bridge_;
    WebHost &web_;
    PlayerWindows &players_;
    DialogCommands &dialogs_;
};

}
