#include "Profile.h"

#include <QDir>

namespace ncm {

Profile::Profile(const RuntimePaths &p)
    : paths(p)
{}

QString Profile::relativeFile(QString path) const
{
    path.replace('\\', '/');
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.split('/').contains(".."))
        return {};
    path.replace('/', '-');
    return paths.webData + "/" + path;
}

}
