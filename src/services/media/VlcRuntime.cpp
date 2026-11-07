#include "VlcRuntime.h"

#include <iterator>

#include <QDir>

namespace ncm {

libvlc_instance_t *createVlcInstance(int argc, const char *const *argv)
{
    static libvlc_instance_t *const bank = [] {
        const char *options[] = {"--no-video", "--no-media-library", "--quiet", "--ignore-config"};
        return libvlc_new(int(std::size(options)), options);
    }();
    return bank ? libvlc_new(argc, argv) : nullptr;
}

libvlc_media_t *openLocalMedia(libvlc_instance_t *instance, const QString &path)
{
    return libvlc_media_new_path(instance, QDir::toNativeSeparators(path).toUtf8().constData());
}

}
