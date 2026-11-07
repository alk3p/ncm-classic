#include "MediaService.h"

#include "AudioService.h"
#include "services/network/MultipartServices.h"
#include "services/network/NativeNetwork.h"
#include "services/transfers/TransferService.h"

namespace ncm {

MediaService::MediaService(Bridge &bridge, const RuntimePaths &paths, WebHost &web, QObject *parent)
    : QObject(parent)
    , network_(std::make_unique<NativeNetwork>(bridge, paths))
    , multipart_(std::make_unique<MultipartServices>(bridge, *network_))
    , audio_(std::make_unique<AudioService>(bridge, *network_, paths, web))
    , transfers_(std::make_unique<TransferService>(bridge, *network_, paths))
{}

MediaService::~MediaService() = default;

}
