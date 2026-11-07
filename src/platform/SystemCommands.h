#pragma once

#include "app/RuntimePaths.h"

#include <QObject>

namespace ncm {

class Bridge;

class SystemCommands final : public QObject
{
public:
    SystemCommands(Bridge &, const RuntimePaths &, QObject *parent = nullptr);

private:
    void watchNetwork(bool);

    Bridge &bridge_;
    QMetaObject::Connection watch_;
};

}
