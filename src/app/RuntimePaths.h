#pragma once

#include <QString>

namespace ncm {

struct RuntimePaths
{
    QString dataRoot;
    QString configRoot;
    QString cacheRoot;
    QString downloadRoot;

    QString settingsFile = configRoot + "/" NCM_APP_ID ".ini";
    QString storageCache = cacheRoot + "/StorageCache";
    QString tempFiles = cacheRoot + "/TempFiles";
    QString webData = dataRoot + "/webdata";
    QString webViewData = dataRoot + "/webview";
};

}
