#pragma once

#include <QString>

#include <vlc/vlc.h>

namespace ncm {

libvlc_instance_t *createVlcInstance(int argc, const char *const *argv);
libvlc_media_t *openLocalMedia(libvlc_instance_t *, const QString &path);

}
