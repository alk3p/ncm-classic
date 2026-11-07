#pragma once

#include <QObject>

namespace ncm {

class Bridge;
class WebHost;

class Mpris final : public QObject
{
public:
    Mpris(Bridge &, WebHost &, QObject *parent = nullptr);
    ~Mpris() override;
};

}
