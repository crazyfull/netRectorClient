#include "clsLoopbackListener.h"
#include "clsLog.h"

namespace netrector {

LoopbackListener::LoopbackListener(EventLoop* loop) : m_v4(loop), m_v6(loop) {}

LoopbackListener::~LoopbackListener()
{
    stop();
}

void LoopbackListener::setReactor(EventLoop* loop)
{
    m_v4.setReactor(loop);
    m_v6.setReactor(loop);
}

bool LoopbackListener::start(int port, EventLoop::acceptCallback cb, void* ctx)
{
    if (m_v4.isListening() || m_v6.isListening())
        return false;

    // IPv4 first: with port 0 it picks the free port that IPv6 then has to use too
    const bool v4 = m_v4.start(port, ipv4Address(), cb, ctx);
    if (v4)
        m_port = m_v4.port();
    else
        m_lastError = m_v4.lastError();

    const bool v6 = m_v6.start(v4 ? m_port : port, ipv6Address(), cb, ctx);
    if (v6) {
        if (!v4)
            m_port = m_v6.port();
    } else {
        if (!v4)
            m_lastError = m_v6.lastError();
        NR_LOG(LOG_INFO, "LoopbackListener: no IPv6 loopback listener on port %d (%s)",
               v4 ? m_port : port, errorString(m_v6.lastError()).c_str());
    }

    if (!v4 && !v6)
        return false;
    if (!v4)
        NR_LOG(LOG_INFO, "LoopbackListener: no IPv4 loopback listener, IPv6 only on port %d", m_port);
    return true;
}

void LoopbackListener::stop()
{
    m_v4.stop();
    m_v6.stop();
}

} // namespace netrector
