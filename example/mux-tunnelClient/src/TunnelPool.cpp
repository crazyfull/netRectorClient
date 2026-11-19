#include "TunnelPool.h"

#include "ClientTunnel.h"

#include "clsLog.h"

#include <algorithm>

using namespace netrector;

TunnelPool::TunnelPool(EventLoop& loop, const ClientConfig& config,
                       const WebSocketClientHandshake& handshake, size_t tunnelCount,
                       TLSContext* tlsContext)
    : m_loop(loop)
    , m_config(config)
    , m_handshake(handshake)
    , m_tlsContext(tlsContext)
    , m_serverName(config.tlsServerName.empty() ? config.handshakeHost : config.tlsServerName)
    , m_slots(tunnelCount)
{
    for (TunnelSlot& slot : m_slots)
        slot.reconnectDelayMs = DefaultClientConfig::ReconnectInitialDelayMs;
}

void TunnelPool::start()
{
    for (size_t index = 0; index < m_slots.size(); index++)
        connectSlot(index);
}

void TunnelPool::stop()
{
    m_stopped = true;
    m_udpWaiters.clear();
}

size_t TunnelPool::connectedCount() const
{
    size_t count = 0;
    for (const TunnelSlot& slot : m_slots) {
        if (slot.tunnel && slot.ready)
            count++;
    }
    return count;
}

uint64_t TunnelPool::nextDeadlineMs() const
{
    uint64_t earliest = 0;
    for (const TunnelSlot& slot : m_slots) {
        if (slot.reconnectScheduled && (earliest == 0 || slot.reconnectAtMs < earliest))
            earliest = slot.reconnectAtMs;
    }
    return earliest;
}

void TunnelPool::tick(uint64_t nowMs)
{
    m_nowMs = nowMs;
    if (m_stopped)
        return;

    checkUdpIdle(nowMs);

    for (size_t index = 0; index < m_slots.size(); index++) {
        TunnelSlot& slot = m_slots[index];
        if (slot.reconnectScheduled && nowMs >= slot.reconnectAtMs) {
            slot.reconnectScheduled = false;
            connectSlot(index);
        }
    }
}

uint32_t TunnelPool::openStream(MultiplexedTunnel::OnStreamDataFn dataFunction,
                                MultiplexedTunnel::OnStreamCloseFn closeFunction,
                                void* argument, MultiplexedTunnel*& tunnel)
{
    const size_t count = m_slots.size();

    for (size_t attempt = 0; attempt < count; attempt++) {
        const size_t index = (m_nextSlot + attempt) % count;
        TunnelSlot& slot = m_slots[index];
        if (!slot.ready)
            continue;

        const uint32_t streamId = slot.tunnel->openStream(dataFunction, closeFunction, argument);
        if (streamId != 0) {
            NR_LOG(LOG_DEBUG, "[client] stream %u assigned to tunnel %zu", streamId, index);
            m_nextSlot = (index + 1) % count;
            tunnel = slot.tunnel;
            return streamId;
        }
        // this tunnel is full or going away: try the next one
    }

    return 0;
}

void TunnelPool::onTunnelReady(ClientTunnel& tunnel)
{
    if (&tunnel == m_udpTunnel) {
        m_udpReady = true;
        m_udpLastActivityMs = m_loop.getCachedNowMs();
        NR_LOG(LOG_INFO, "[client] UDP tunnel ready (%zu flow(s) waiting)", m_udpWaiters.size());
        std::vector<UdpTunnelWaiter*> waiters;
        waiters.swap(m_udpWaiters);
        for (UdpTunnelWaiter* waiter : waiters)
            waiter->onUdpTunnelReady();
        return;
    }

    size_t index = 0;
    TunnelSlot* slot = findSlot(tunnel, index);
    if (!slot)
        return;

    slot->ready = true;
    slot->reconnectDelayMs = DefaultClientConfig::ReconnectInitialDelayMs;
}

void TunnelPool::onTunnelClosed(ClientTunnel& tunnel)
{
    if (&tunnel == m_udpTunnel) {
        const bool wasReady = m_udpReady;
        m_udpTunnel = nullptr;
        m_udpReady = false;
        if (!wasReady)
            m_udpNextAttemptMs = m_loop.getCachedNowMs() + static_cast<uint64_t>(DefaultClientConfig::UdpRetryDelayMs);
        NR_LOG(LOG_INFO, "[client] UDP tunnel closed");
        failUdpWaiters();       // only flows that were still waiting
        return;
    }

    size_t index = 0;
    TunnelSlot* slot = findSlot(tunnel, index);
    if (!slot)
        return;     // a tunnel that was already replaced after a GoAway

    NR_LOG(LOG_INFO, "[client] tunnel %zu lost", index);
    slot->tunnel = nullptr;
    slot->ready = false;
    scheduleReconnect(index);
}

void TunnelPool::onTunnelGoAway(ClientTunnel& tunnel)
{
    if (&tunnel == m_udpTunnel) {
        // no new flows on it; it ends with its streams. The next flow connects a new one.
        NR_LOG(LOG_INFO, "[client] UDP tunnel received GoAway");
        m_udpTunnel = nullptr;
        m_udpReady = false;
        return;
    }

    size_t index = 0;
    TunnelSlot* slot = findSlot(tunnel, index);
    if (!slot)
        return;

    // The server accepts no new streams on this tunnel. It keeps running until its streams end
    // and is deleted by the reactor when it closes; meanwhile a replacement is connected.
    NR_LOG(LOG_INFO, "[client] tunnel %zu received GoAway, replacing it", index);
    slot->tunnel = nullptr;
    slot->ready = false;
    scheduleReconnect(index);
}

void TunnelPool::connectSlot(size_t index)
{
    TunnelSlot& slot = m_slots[index];
    if (m_stopped || slot.tunnel)
        return;

    ClientTunnel* tunnel = new ClientTunnel(*this, m_config, m_handshake);
    tunnel->setReactor(&m_loop);

    // With TLS the handshake request is sent after the TLS handshake (ClientTunnel::handleOnConnected).
    if (m_tlsContext && !tunnel->enableTLS(m_tlsContext, false, m_serverName.c_str())) {
        NR_LOG(LOG_ERROR, "[client] cannot enable TLS on tunnel %zu", index);
        delete tunnel;
        scheduleReconnect(index);
        return;
    }

    slot.tunnel = tunnel;
    slot.ready = false;

    NR_LOG(LOG_DEBUG, "[client] connecting tunnel %zu to %s:%u", index, m_config.cdnAddress.c_str(), m_config.cdnPort);
    if (!tunnel->connectTo(m_config.cdnAddress.c_str(), m_config.cdnPort)) {
        // false: no callback was or will be called, the object is still ours
        slot.tunnel = nullptr;
        delete tunnel;
        scheduleReconnect(index);
    }
    // true: the result arrives through the tunnel callbacks (possibly before connectTo returns)
}

void TunnelPool::scheduleReconnect(size_t index)
{
    TunnelSlot& slot = m_slots[index];
    if (m_stopped || slot.reconnectScheduled || slot.tunnel)
        return;

    slot.reconnectScheduled = true;
    const int delay = slot.reconnectDelayMs;
    NR_LOG(LOG_DEBUG, "[client] reconnecting tunnel %zu in %d ms", index, delay);

    slot.reconnectAtMs = (m_nowMs ? m_nowMs : m_loop.getCachedNowMs()) + static_cast<uint64_t>(delay);

    slot.reconnectDelayMs = std::min(delay * 2, DefaultClientConfig::ReconnectMaxDelayMs);
}

TunnelSlot* TunnelPool::findSlot(ClientTunnel& tunnel, size_t& index)
{
    for (size_t i = 0; i < m_slots.size(); i++) {
        if (m_slots[i].tunnel == &tunnel) {
            index = i;
            return &m_slots[i];
        }
    }
    return nullptr;
}

// ---- UDP tunnel ----

bool TunnelPool::requestUdpTunnel(UdpTunnelWaiter* waiter)
{
    if (!m_config.udpEnabled || m_stopped)
        return false;

    if (m_udpTunnel && m_udpReady) {
        waiter->onUdpTunnelReady();
        return true;
    }

    if (!m_udpTunnel) {
        if (m_loop.getCachedNowMs() < m_udpNextAttemptMs)
            return false;       // just failed: do not hammer the server
        m_udpWaiters.push_back(waiter);
        connectUdpTunnel();     // may fail at once and call failUdpWaiters()
        return true;
    }

    m_udpWaiters.push_back(waiter);     // connecting
    return true;
}

void TunnelPool::cancelUdpWait(UdpTunnelWaiter* waiter)
{
    m_udpWaiters.erase(std::remove(m_udpWaiters.begin(), m_udpWaiters.end(), waiter), m_udpWaiters.end());
}

void TunnelPool::connectUdpTunnel()
{
    ClientTunnel* tunnel = new ClientTunnel(*this, m_config, m_handshake);
    tunnel->setReactor(&m_loop);
    tunnel->setKeepAlive(m_config.udpKeepAliveSeconds);
    tunnel->setMaxStreams(m_config.udpMaxStreams);

    if (m_tlsContext && !tunnel->enableTLS(m_tlsContext, false, m_serverName.c_str())) {
        NR_LOG(LOG_ERROR, "[client] cannot enable TLS on the UDP tunnel");
        delete tunnel;
        m_udpNextAttemptMs = m_loop.getCachedNowMs() + static_cast<uint64_t>(DefaultClientConfig::UdpRetryDelayMs);
        failUdpWaiters();
        return;
    }

    m_udpTunnel = tunnel;
    m_udpReady = false;
    m_udpLastActivityMs = m_loop.getCachedNowMs();

    NR_LOG(LOG_INFO, "[client] connecting the UDP tunnel to %s:%u", m_config.cdnAddress.c_str(), m_config.cdnPort);
    if (!tunnel->connectTo(m_config.cdnAddress.c_str(), m_config.cdnPort)) {
        m_udpTunnel = nullptr;
        delete tunnel;
        m_udpNextAttemptMs = m_loop.getCachedNowMs() + static_cast<uint64_t>(DefaultClientConfig::UdpRetryDelayMs);
        failUdpWaiters();
    }
}

void TunnelPool::failUdpWaiters()
{
    std::vector<UdpTunnelWaiter*> waiters;
    waiters.swap(m_udpWaiters);
    for (UdpTunnelWaiter* waiter : waiters)
        waiter->onUdpTunnelFailed();
}

uint32_t TunnelPool::openUdpStream(MultiplexedTunnel::OnStreamDataFn dataFunction,
                                   MultiplexedTunnel::OnStreamCloseFn closeFunction,
                                   void* argument, MultiplexedTunnel*& tunnel)
{
    if (!m_udpTunnel || !m_udpReady)
        return 0;

    const uint32_t streamId = m_udpTunnel->openStream(dataFunction, closeFunction, argument);
    if (streamId != 0) {
        tunnel = m_udpTunnel;
        m_udpLastActivityMs = m_loop.getCachedNowMs();
    }
    return streamId;
}

void TunnelPool::noteUdpActivity()
{
    m_udpLastActivityMs = m_loop.getCachedNowMs();
}

void TunnelPool::setScreenOff(bool screenOff)
{
    if (m_screenOff == screenOff)
        return;
    m_screenOff = screenOff;
    NR_LOG(LOG_INFO, "[client] screen %s", screenOff ? "off" : "on");
    // the idle limit changes: checkUdpIdle() applies it on the next pass
}

void TunnelPool::checkUdpIdle(uint64_t nowMs)
{
    if (!m_udpTunnel || !m_udpReady)
        return;

    const uint32_t limitSeconds = m_screenOff ? m_config.udpIdleScreenOffSeconds : m_config.udpIdleSeconds;
    if (limitSeconds == 0)
        return;

    if (nowMs >= m_udpLastActivityMs && nowMs - m_udpLastActivityMs >= static_cast<uint64_t>(limitSeconds) * 1000)
        closeUdpTunnel(m_screenOff ? "idle with the screen off" : "idle");
}

void TunnelPool::closeUdpTunnel(const char* reason)
{
    if (!m_udpTunnel)
        return;
    NR_LOG(LOG_INFO, "[client] closing the UDP tunnel (%s)", reason);

    // The tunnel closes its streams, which ends the UDP sessions of the apps; the next UDP flow
    // connects a new tunnel. onTunnelClosed() clears the pointer.
    ClientTunnel* tunnel = m_udpTunnel;
    m_udpLastActivityMs = m_loop.getCachedNowMs();      // no repeated attempt if the close is deferred
    tunnel->close(true);
}
