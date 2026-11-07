#pragma once

#include <memory>

#include <QObject>

namespace ncm {

class AudioService;
class Bridge;
class MultipartServices;
class NativeNetwork;
class TransferService;
class WebHost;
struct RuntimePaths;

class MediaService final : public QObject
{
    Q_OBJECT

public:
    MediaService(Bridge &, const RuntimePaths &, WebHost &, QObject *parent = nullptr);
    ~MediaService() override;

    NativeNetwork &network() { return *network_; }

private:
    std::unique_ptr<NativeNetwork> network_;
    std::unique_ptr<MultipartServices> multipart_;
    std::unique_ptr<AudioService> audio_;
    std::unique_ptr<TransferService> transfers_;
};

}
