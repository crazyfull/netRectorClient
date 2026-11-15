#include "ClientApplication.h"

#include "LocalSocksConnection.h"
#include "TerminationSignal.h"
#include "TunnelPool.h"
#include "raiseFileDescriptorLimit.h"

#include "clsEventLoop.h"
#include "clsListener.h"
#include "clsLoopbackListener.h"
#include "clsLog.h"
#include "clsTLSContext.h"

#include <algorithm>

using namespace netrector;

namespace {

bool isLoopbackAddress(const std::string& address)
{
    return address == "127.0.0.1" || address == "::1" || address == "localhost";
}

} // namespace

ClientApplication::ClientApplication(const ClientConfig& config)
    : m_config(config)
    , m_handshake(m_config)
{
}

TCPSocket* ClientApplication::createLocalConnection(void* context)
{
    ClientApplication* application = static_cast<ClientApplication*>(context);
    return new LocalSocksConnection(application->m_config);
}

void ClientApplication::requestStop()
{
    m_stopRequested = true;
    std::lock_guard<std::mutex> lock(m_loopMutex);
    if (m_loop)
        m_loop->wake();
}

int ClientApplication::run(bool handleSignals)
{
    if (handleSignals)
        installTerminationHandler();
    setLogLevel(m_config.verbose ? LOG_DEBUG : LOG_INFO);

    const int maxConnections = raiseFileDescriptorLimit();

    // Declared before the loop so that it outlives it: while the loop closes its sockets they call
    // back into the pool.
    std::unique_ptr<TunnelPool> pool;

    // TLS context shared by all tunnels; it must outlive every socket, so it is declared before the loop too.
    std::unique_ptr<TLSContext> tlsContext;
    if (m_config.useTls) {
        std::string error;
        tlsContext.reset(TLSContext::createClient(m_config.tlsVerifyPeer,
                                                  m_config.tlsCaFile.empty() ? nullptr : m_config.tlsCaFile.c_str(), &error));
        if (!tlsContext) {
            NR_LOG(LOG_ERROR, "[client] TLS is not available: %s", error.c_str());
            return 1;
        }
        NR_LOG(LOG_INFO, "[client] TLS enabled, server certificate %s",
               m_config.tlsVerifyPeer ? "is verified" : "is NOT verified");
    }

    EventLoop loop;
    {
        std::lock_guard<std::mutex> lock(m_loopMutex);
        m_loop = &loop;
    }
    loop.setMaxConnections(static_cast<uint32_t>(maxConnections));

    pool = std::make_unique<TunnelPool>(loop, m_config, m_handshake, static_cast<size_t>(m_config.tunnelCount), tlsContext.get());
    loop.setUserData(pool.get());

    // A loopback address means "only this device": listen on IPv4 and IPv6 loopback together, so
    // a SOCKS5 client may connect to either (hev-socks5-tunnel on iOS prefers IPv6).
    const bool loopbackOnly = isLoopbackAddress(m_config.listenAddress);
    Listener listener;
    LoopbackListener loopbackListener;
    listener.setReactor(&loop);
    loopbackListener.setReactor(&loop);
    const bool listening = loopbackOnly
        ? loopbackListener.start(m_config.listenPort, &ClientApplication::createLocalConnection, this)
        : listener.start(m_config.listenPort, m_config.listenAddress.c_str(), &ClientApplication::createLocalConnection, this);
    if (!listening) {
        NR_LOG(LOG_ERROR, "[client] cannot listen on %s:%u", m_config.listenAddress.c_str(), m_config.listenPort);
        std::lock_guard<std::mutex> lock(m_loopMutex);
        m_loop = nullptr;
        return 1;
    }

    pool->start();

    if (loopbackOnly) {
        NR_LOG(LOG_INFO, "[client] SOCKS5 on port %d (IPv4 loopback: %s, IPv6 loopback: %s), %d tunnel(s) to %s:%u",
               loopbackListener.port(), loopbackListener.ipv4Active() ? "yes" : "no",
               loopbackListener.ipv6Active() ? "yes" : "no", m_config.tunnelCount,
               m_config.cdnAddress.c_str(), m_config.cdnPort);
    } else {
        NR_LOG(LOG_INFO, "[client] SOCKS5 on %s:%u, %d tunnel(s) to %s:%u",
               m_config.listenAddress.c_str(), m_config.listenPort, m_config.tunnelCount,
               m_config.cdnAddress.c_str(), m_config.cdnPort);
    }

    // netRectorClient has no timers: the application drives the loop in short slices and runs its
    // own periodic work (tunnel reconnects) after each one.
    while (!m_stopRequested && !(handleSignals && isTerminationRequested())) {
        // sleep until the next reconnect is due, at most MaxLoopSliceMs
        int sliceMs = DefaultClientConfig::MaxLoopSliceMs;
        const uint64_t deadline = pool->nextDeadlineMs();
        if (deadline != 0) {
            const uint64_t now = loop.getCachedNowMs();
            const int untilDeadline = deadline > now ? static_cast<int>(std::min<uint64_t>(deadline - now, sliceMs)) : 0;
            sliceMs = std::max(untilDeadline, DefaultClientConfig::MinLoopSliceMs);
        }
        loop.runOnce(sliceMs);
        loop.updateCashedTime();
        pool->tick(loop.getCachedNowMs());
        m_connectedTunnels = static_cast<int>(pool->connectedCount());
    }

    NR_LOG(LOG_INFO, "[client] shutting down");

    pool->stop();       // no reconnects while the sockets are closed
    m_connectedTunnels = 0;
    listener.stop();
    loopbackListener.stop();
    {
        std::lock_guard<std::mutex> lock(m_loopMutex);
        m_loop = nullptr;       // after this requestStop() no longer touches the loop
    }
    loop.closeAll();
    return 0;
}
