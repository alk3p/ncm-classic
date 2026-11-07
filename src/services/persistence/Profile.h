#pragma once

#include "app/RuntimePaths.h"

#include <QString>

namespace ncm {

struct Profile
{
    explicit Profile(const RuntimePaths &paths);

    QString relativeFile(QString path) const;

    RuntimePaths paths;
};

}
