#include "MuxTunnelCore.h"

#include "ClientApplication.h"
#include "ClientConfig.h"

#include <mutex>
#include <string>

namespace {

std::mutex gMutex;
ClientApplication* gApplication = nullptr;     // set while muxTunnelCoreRun() is active
bool gStopRequested = false;                   // a stop that arrived before the run registered itself

std::string toString(const char* text)
{
    return text ? std::string(text) : std::string();
}

} // namespace

extern "C" {

void muxTunnelCoreReset(void)
{
    std::lock_guard<std::mutex> lock(gMutex);
    gStopRequested = false;
}

int32_t muxTunnelCoreRun(const MuxTunnelCoreConfig* source)
{
    if (!source)
        return muxTunnelCoreResultInvalidConfig;

    ClientConfig config;
    config.listenAddress = toString(source->listenAddress);
    config.listenPort = source->listenPort;
    config.cdnAddress = toString(source->cdnAddress);
    config.cdnPort = source->cdnPort;
    config.tunnelCount = source->tunnelCount;
    config.handshakeHost = toString(source->handshakeHost);
    config.handshakePath = toString(source->handshakePath);
    config.webSocketKey = toString(source->webSocketKey);
    config.expectedWebSocketAccept = toString(source->expectedAccept);
    config.keepAliveSeconds = source->keepAliveSeconds;
    config.verbose = source->verbose != 0;

    if (config.cdnAddress.empty() || config.listenAddress.empty())
        return muxTunnelCoreResultInvalidConfig;
    if (config.tunnelCount < DefaultClientConfig::MinTunnelCount)
        config.tunnelCount = DefaultClientConfig::MinTunnelCount;
    if (config.tunnelCount > DefaultClientConfig::MaxTunnelCount)
        config.tunnelCount = DefaultClientConfig::MaxTunnelCount;

    ClientApplication application(config);
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gApplication = &application;
        if (gStopRequested)
            application.requestStop();
    }

    const int code = application.run(false);    // no signal handlers inside an extension process

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gApplication = nullptr;
    }
    return code;
}

void muxTunnelCoreStop(void)
{
    std::lock_guard<std::mutex> lock(gMutex);
    gStopRequested = true;
    if (gApplication)
        gApplication->requestStop();
}

int32_t muxTunnelCoreConnectedTunnels(void)
{
    std::lock_guard<std::mutex> lock(gMutex);
    return gApplication ? gApplication->connectedTunnelCount() : 0;
}

} // extern "C"
