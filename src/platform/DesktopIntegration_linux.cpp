#include "DesktopIntegration.h"

#include "bridge/Bridge.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

namespace ncm {

namespace {

constexpr auto desktopId = NCM_APP_ID ".desktop";
constexpr const char *audioTypes[]
    = {"audio/aac",
       "audio/flac",
       "audio/mp3",
       "audio/mp4",
       "audio/mpeg",
       "audio/ogg",
       "audio/x-ape",
       "audio/x-flac",
       "audio/x-mp3",
       "audio/x-mpeg",
       "audio/x-ms-wma",
       "audio/x-vorbis",
       "audio/x-wav"};
constexpr const char *associationGroups[] = {"[Default Applications]", "[Added Associations]"};

QString configFile(const QString &name)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + '/' + name;
}

QString execArgument(QString path)
{
    path.replace('\\', R"(\\\\)");
    for (const auto special : {'"', '`', '$'})
        path.replace(special, R"(\\)" + QString(special));
    path.replace('%', "%%");
    return '"' + path + '"';
}

bool saveDesktopEntry(const QString &path)
{
    QFile source(":/ncm/" NCM_APP_ID ".desktop");
    if (!source.open(QIODevice::ReadOnly))
        return false;

    auto lines = source.readAll().split('\n');
    for (auto &line : lines) {
        if (line.startsWith("Exec="))
            line = "Exec=" + execArgument(QCoreApplication::applicationFilePath()).toUtf8() + " %U";
    }
    const auto entry = lines.join('\n');
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(entry) == entry.size() && file.commit();
}

class MimeApps
{
public:
    MimeApps()
    {
        QFile file(configFile("mimeapps.list"));
        if (file.open(QIODevice::ReadOnly))
            lines_ = QString::fromUtf8(file.readAll()).split('\n');
    }

    QStringList applications(const QString &group, const QString &type) const
    {
        const auto at = find(group, type);
        return at < 0 ? QStringList() : lines_[at].section('=', 1).split(';', Qt::SkipEmptyParts);
    }

    void setApplications(
        const QString &groupName, const QString &type, const QStringList &applications)
    {
        const auto at = find(groupName, type);
        if (applications.isEmpty()) {
            if (at >= 0)
                lines_.removeAt(at);
            return;
        }
        const auto line = type + '=' + applications.join(';') + ';';
        if (at >= 0) {
            lines_[at] = line;
            return;
        }
        auto group = lines_.indexOf(groupName);
        if (group < 0) {
            if (!lines_.isEmpty() && !lines_.constLast().isEmpty())
                lines_.append(QString());
            lines_.append(groupName);
            group = lines_.size() - 1;
        }
        auto end = groupEnd(group);
        while (end > group + 1 && lines_[end - 1].trimmed().isEmpty())
            --end;
        lines_.insert(end, line);
    }

    bool save() const
    {
        auto text = lines_.join('\n');
        if (!text.endsWith('\n'))
            text += '\n';
        const auto bytes = text.toUtf8();
        QSaveFile file(configFile("mimeapps.list"));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
               && file.commit();
    }

private:
    qsizetype groupEnd(qsizetype group) const
    {
        auto end = group + 1;
        while (end < lines_.size() && !lines_[end].startsWith('['))
            ++end;
        return end;
    }

    qsizetype find(const QString &groupName, const QString &type) const
    {
        const auto group = lines_.indexOf(groupName);
        if (group < 0)
            return -1;
        for (auto i = group + 1, end = groupEnd(group); i < end; ++i)
            if (lines_[i].section('=', 0, 0).trimmed() == type)
                return i;
        return -1;
    }

    QStringList lines_;
};

bool setDefault(bool install)
{
    MimeApps apps;
    for (const auto group : associationGroups)
        for (const auto type : audioTypes) {
            auto applications = apps.applications(group, type);
            applications.removeAll(desktopId);
            if (install)
                applications.prepend(desktopId);
            apps.setApplications(group, type, applications);
        }
    return apps.save();
}

bool isDefault()
{
    const MimeApps apps;
    return std::ranges::all_of(audioTypes, [&apps](const char *type) {
        return apps.applications(associationGroups[0], type).value(0) == desktopId;
    });
}

}

void DesktopIntegration::registerAutostart()
{
    const auto entry = configFile("autostart/" + QString(desktopId));
    bridge_.addHandler("app.getAutoRunState", [entry](const QJsonArray &, Bridge::Reply reply) {
        reply({QFile::exists(entry)});
    });
    bridge_.addHandler("app.setAutoRun", [entry](const QJsonArray &, Bridge::Reply reply) {
        reply({saveDesktopEntry(entry)});
    });
    bridge_.addHandler("app.cancelAutoRun", [entry](const QJsonArray &, Bridge::Reply reply) {
        reply({!QFile::exists(entry) || QFile::remove(entry)});
    });
}

void DesktopIntegration::registerDefaultClient()
{
    bridge_.addHandler("app.isRegisterDefaultClient", [](const QJsonArray &, Bridge::Reply reply) {
        reply({isDefault()});
    });
    bridge_.addHandler("app.registerDefaultClient", [](const QJsonArray &, Bridge::Reply reply) {
        reply({setDefault(true)});
    });
    bridge_.addHandler("app.unRegisterDefaultClient", [](const QJsonArray &, Bridge::Reply reply) {
        reply({setDefault(false)});
    });
}

}
