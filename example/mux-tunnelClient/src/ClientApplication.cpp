#include "ClientApplication.h"

#include "LocalSocksConnection.h"
#include "TerminationSignal.h"
#include "TunnelPool.h"
#include "raiseFileDescriptorLimit.h"

#include "clsEventLoop.h"
#include "clsListener.h"
#include "clsLog.h"

using namespace netrector;

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

int ClientApplication::run()
{
    installTerminationHandler();
    setLogLevel(m_config.verbose ? LOG_DEBUG : LOG_INFO);

    const int maxConnections = raiseFileDescriptorLimit();

    // Declared before the loop so that it outlives it: while the loop closes its sockets they call
    // back into the pool.
    std::unique_ptr<TunnelPool> pool;

    EventLoop loop;
    loop.setMaxConnections(static_cast<uint32_t>(maxConnections));

    pool = std::make_unique<TunnelPool>(loop, m_config, m_handshake, static_cast<size_t>(m_config.tunnelCount));
    loop.setUserData(pool.get());

    Listener listener;
    listener.setReactor(&loop);
    if (!listener.start(m_config.listenPort, m_config.listenAddress.c_str(), &ClientApplication::createLocalConnection, this)) {
        NR_LOG(LOG_ERROR, "[client] cannot listen on %s:%u", m_config.listenAddress.c_str(), m_config.listenPort);
        return 1;
    }

    pool->start();

    NR_LOG(LOG_INFO, "[client] SOCKS5 on %s:%u, %d tunnel(s) to %s:%u",
           m_config.listenAddress.c_str(), m_config.listenPort, m_config.tunnelCount,
           m_config.cdnAddress.c_str(), m_config.cdnPort);

    // netRectorClient has no timers: the application drives the loop in short slices and runs its
    // own periodic work (tunnel reconnects) after each one.
    while (!isTerminationRequested()) {
        loop.runOnce(DefaultClientConfig::LoopSliceMs);
        pool->tick(loop.getCachedNowMs());
    }

    NR_LOG(LOG_INFO, "[client] signal %d received, shutting down", receivedTerminationSignal());

    pool->stop();       // no reconnects while the sockets are closed
    listener.stop();
    loop.closeAll();
    return 0;
}
