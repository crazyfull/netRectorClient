// JNI bridge between the Android app and the mux-tunnelClient core (ClientApplication).
//
//   MuxTunnelNative.nativeRun(...)   blocks the calling (Java) thread until nativeStop() is called
//   MuxTunnelNative.nativeStop()     thread-safe
//
// The SOCKS5 listener started by nativeRun() is what hev-socks5-tunnel (tun2socks) connects to.

#include "ClientApplication.h"
#include "ClientConfig.h"

#include <jni.h>

#include <mutex>
#include <string>

namespace {

std::mutex g_mutex;
ClientApplication* g_application = nullptr;    // set while nativeRun() is active
bool g_stopRequested = false;                  // a stop that arrived before nativeRun() registered

std::string toString(JNIEnv* env, jstring value)
{
    if (!value)
        return std::string();
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars)
        return std::string();
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_io_github_crazyfull_muxtunnel_MuxTunnelNative_nativeReset(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stopRequested = false;
}

// Returns the exit code of ClientApplication::run(): 0 clean stop, 1 the SOCKS5 port cannot be opened.
JNIEXPORT jint JNICALL
Java_io_github_crazyfull_muxtunnel_MuxTunnelNative_nativeRun(
    JNIEnv* env, jclass, jstring listenAddress, jint listenPort, jstring cdnAddress, jint cdnPort,
    jint tunnelCount, jstring handshakeHost, jstring handshakePath, jstring webSocketKey,
    jstring expectedAccept, jint keepAliveSeconds, jboolean verbose)
{
    ClientConfig config;
    config.listenAddress = toString(env, listenAddress);
    config.listenPort = static_cast<uint16_t>(listenPort);
    config.cdnAddress = toString(env, cdnAddress);
    config.cdnPort = static_cast<uint16_t>(cdnPort);
    config.tunnelCount = tunnelCount;
    config.handshakeHost = toString(env, handshakeHost);
    config.handshakePath = toString(env, handshakePath);
    config.webSocketKey = toString(env, webSocketKey);
    config.expectedWebSocketAccept = toString(env, expectedAccept);
    config.keepAliveSeconds = static_cast<uint32_t>(keepAliveSeconds < 0 ? 0 : keepAliveSeconds);
    config.verbose = verbose == JNI_TRUE;

    if (config.tunnelCount < DefaultClientConfig::MinTunnelCount)
        config.tunnelCount = DefaultClientConfig::MinTunnelCount;
    if (config.tunnelCount > DefaultClientConfig::MaxTunnelCount)
        config.tunnelCount = DefaultClientConfig::MaxTunnelCount;

    ClientApplication application(config);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_application = &application;
        if (g_stopRequested)
            application.requestStop();
    }

    const int code = application.run(false);    // no signal handlers inside an Android process

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_application = nullptr;
    }
    return code;
}

JNIEXPORT void JNICALL
Java_io_github_crazyfull_muxtunnel_MuxTunnelNative_nativeStop(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stopRequested = true;
    if (g_application)
        g_application->requestStop();
}

} // extern "C"
