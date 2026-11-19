#include "MuxTunnelCore.h"

#include "ClientApplication.h"
#include "ClientConfig.h"
#include "clsLog.h"

#include <mutex>
#include <string>

namespace {

std::mutex gMutex;
ClientApplication* gApplication = nullptr;     // set while muxTunnelCoreRun() is active
bool gStopRequested = false;                   // a stop that arrived before the run registered itself

MuxTunnelCoreLogFunction gLogFunction = nullptr;

void forwardLog(int level, const char* message)
{
    MuxTunnelCoreLogFunction function = gLogFunction;
    if (function)
        function(level, message);
}

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
    config.sniTLSBypass = source->sniBypass != 0;
    config.udpEnabled = source->udpEnabled != 0;
    config.udpIdleSeconds = source->udpIdleSeconds;
    config.udpIdleScreenOffSeconds = source->udpIdleScreenOffSeconds;
    config.useTls = source->useTls != 0;        // SNI = the handshake host (tlsServerName stays empty)

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

void muxTunnelCoreSetLogFunction(MuxTunnelCoreLogFunction function)
{
    gLogFunction = function;
    netrector::setLogHandler(function ? forwardLog : nullptr);
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

int32_t muxTunnelCoreUdpTunnelUp(void)
{
    std::lock_guard<std::mutex> lock(gMutex);
    return gApplication ? gApplication->udpTunnelUp() : 0;
}

void muxTunnelCoreSetScreenOff(int32_t screenOff)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (gApplication)
        gApplication->setScreenOff(screenOff != 0);
}

} // extern "C"
