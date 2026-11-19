// JNI bridge between the Android app and the mux-tunnelClient core (ClientApplication).
//
//   MuxTunnelNative.nativeRun(...)   blocks the calling (Java) thread until nativeStop() is called
//   MuxTunnelNative.nativeStop()     thread-safe
//
// The SOCKS5 listener started by nativeRun() is what hev-socks5-tunnel (tun2socks) connects to.

#include "ClientApplication.h"
#include "ClientConfig.h"
#include "clsLog.h"

#include <android/log.h>
#include <jni.h>
#include <sys/time.h>

#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

namespace {

std::mutex g_mutex;
ClientApplication* g_application = nullptr;    // set while nativeRun() is active
bool g_stopRequested = false;                  // a stop that arrived before nativeRun() registered

// The core's log lines, kept here so the app can show them (Diagnostics) without adb. They also go to logcat.
std::mutex g_logMutex;
std::deque<std::string> g_logLines;
const size_t MaxLogLines = 800;

void logHandler(int level, const char* message)
{
    static const char letters[] = { 'E', 'W', 'I', 'D' };
    static const int priorities[] = { ANDROID_LOG_ERROR, ANDROID_LOG_WARN, ANDROID_LOG_INFO, ANDROID_LOG_DEBUG };
    const int index = level & 3;
    __android_log_write(priorities[index], "netRectorClient", message);

    timeval now;
    gettimeofday(&now, nullptr);
    time_t seconds = now.tv_sec;
    tm local;
    localtime_r(&seconds, &local);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d %c ", local.tm_hour, local.tm_min, local.tm_sec,
             static_cast<int>(now.tv_usec / 1000), letters[index]);

    std::string line(stamp);
    for (const char* c = message; *c; ++c)      // plain ASCII only: the text goes through NewStringUTF
        line += (*c >= 0x20 && *c < 0x7f) ? *c : '?';

    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logLines.push_back(line);
    if (g_logLines.size() > MaxLogLines)
        g_logLines.pop_front();
}

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
    netrector::setLogHandler(logHandler);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stopRequested = false;
}

// The core's log (newest last), one line per message.
JNIEXPORT jstring JNICALL
Java_io_github_crazyfull_muxtunnel_MuxTunnelNative_nativeGetLog(JNIEnv* env, jclass)
{
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        for (const std::string& line : g_logLines) {
            text += line;
            text += '\n';
        }
    }
    return env->NewStringUTF(text.c_str());
}

JNIEXPORT void JNICALL
Java_io_github_crazyfull_muxtunnel_MuxTunnelNative_nativeClearLog(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logLines.clear();
}

// Returns the exit code of ClientApplication::run(): 0 clean stop, 1 the SOCKS5 port cannot be opened.
JNIEXPORT jint JNICALL
Java_io_github_crazyfull_muxtunnel_MuxTunnelNative_nativeRun(
    JNIEnv* env, jclass, jstring listenAddress, jint listenPort, jstring cdnAddress, jint cdnPort,
    jint tunnelCount, jstring handshakeHost, jstring handshakePath, jstring webSocketKey,
    jstring expectedAccept, jint keepAliveSeconds, jboolean verbose, jboolean useTls,
    jboolean udpEnabled, jint udpIdleSeconds, jint udpIdleScreenOffSeconds, jboolean sniBypass)
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
    config.useTls = useTls == JNI_TRUE;     // SNI = the handshake host (tlsServerName stays empty)

    config.sniTLSBypass = sniBypass == JNI_TRUE;
    config.udpEnabled = udpEnabled == JNI_TRUE;
    config.udpIdleSeconds = static_cast<uint32_t>(udpIdleSeconds < 0 ? 0 : udpIdleSeconds);
    config.udpIdleScreenOffSeconds = static_cast<uint32_t>(udpIdleScreenOffSeconds < 0 ? 0 : udpIdleScreenOffSeconds);

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

// Number of mux tunnels that are connected (handshake done) right now; 0 when the core is not running.
JNIEXPORT jint JNICALL
Java_io_github_crazyfull_muxtunnel_MuxTunnelNative_nativeConnectedTunnels(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_application ? static_cast<jint>(g_application->connectedTunnelCount()) : 0;
}

// 1 while the UDP tunnel is connected (or connecting); always 0 when UDP is off.
JNIEXPORT jint JNICALL
Java_io_github_crazyfull_muxtunnel_MuxTunnelNative_nativeUdpTunnelUp(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_application ? static_cast<jint>(g_application->udpTunnelUp()) : 0;
}

// The screen turned off / on: with UDP the shorter screen-off idle time applies.
JNIEXPORT void JNICALL
Java_io_github_crazyfull_muxtunnel_MuxTunnelNative_nativeSetScreenOff(JNIEnv*, jclass, jboolean screenOff)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_application)
        g_application->setScreenOff(screenOff == JNI_TRUE);
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
