#include "LocalSocksConnection.h"

#include "Socks5GreetingParser.h"
#include "Socks5Reply.h"

#include "clsEventLoop.h"
#include "clsLog.h"

using namespace netrector;

LocalSocksConnection::LocalSocksConnection(const ClientConfig& config)
    : m_config(config)
{
}

void LocalSocksConnection::onAccepted()
{
    setAllowHalfClose(true);    // FIN of the application becomes FIN of the stream and vice versa
    setIdleTimeout(m_config.localGreetingTimeoutSeconds);
    m_pool = static_cast<TunnelPool*>(getReactor()->userData());
}

void LocalSocksConnection::onReceiveData(const uint8_t* data, size_t length)
{
    switch (m_state) {
    case State::Relaying:
        if (m_streamId != 0) {
            if (m_udp)
                m_pool->noteUdpActivity();
            m_tunnel->sendToStream(m_streamId, data, length);
        }
        break;

    case State::AwaitingGreeting:
        m_greetingBuffer.append(reinterpret_cast<const char*>(data), length);
        handleGreeting();
        break;

    case State::AwaitingRequest:
        handleRequestBytes(data, length);
        break;

    case State::WaitingUdpTunnel:
        m_greetingBuffer.append(reinterpret_cast<const char*>(data), length);   // sent when the tunnel is up
        break;

    case State::Failed:
        break;
    }
}

// The application will not send anything more.
void LocalSocksConnection::onReadEOF()
{
    if (m_streamId != 0) {
        if (!m_finSentToStream) {
            m_finSentToStream = true;
            m_tunnel->closeStream(m_streamId, false);   // FIN, receiving continues
        }
    } else {
        close();
    }
}

// The send queue towards the application is full: stop granting window to the server side.
void LocalSocksConnection::onPause()
{
    if (m_streamId != 0)
        m_tunnel->pauseStream(m_streamId);
}

void LocalSocksConnection::onResume()
{
    if (m_streamId != 0)
        m_tunnel->resumeStream(m_streamId);
}

void LocalSocksConnection::onClose()
{
    if (m_waitingForUdpTunnel) {
        m_waitingForUdpTunnel = false;
        m_pool->cancelUdpWait(this);
    }

    if (m_streamId == 0)
        return;

    const uint32_t streamId = m_streamId;
    m_streamId = 0;

    if (m_finSentToStream && m_streamFinReceived) {
        // both directions finished: let the tunnel end the stream
        m_tunnel->detachStream(streamId);
    } else {
        // the application broke the connection: reset the stream
        m_tunnel->closeStream(streamId, true);
    }
}

void LocalSocksConnection::handleGreeting()
{
    size_t consumed = 0;
    bool noAuthOffered = false;
    const Socks5ParseResult result = Socks5GreetingParser::parse(
        reinterpret_cast<const uint8_t*>(m_greetingBuffer.data()), m_greetingBuffer.size(), consumed, noAuthOffered);

    if (result == Socks5ParseResult::Incomplete)
        return;

    NR_LOG(LOG_DEBUG, "[client] SOCKS5 greeting received (%s, no-auth offered: %s)",
           result == Socks5ParseResult::Invalid ? "invalid" : "valid", noAuthOffered ? "yes" : "no");

    if (result == Socks5ParseResult::Invalid) {
        m_state = State::Failed;
        close(true);
        return;
    }

    m_greetingBuffer.erase(0, consumed);

    const uint8_t response[2] = { 0x05, static_cast<uint8_t>(noAuthOffered ? 0x00 : 0xFF) };
    send(response, sizeof(response));

    if (!noAuthOffered) {
        m_state = State::Failed;
        close();    // the answer is flushed first
        return;
    }

    m_state = State::AwaitingRequest;

    // request bytes that were pipelined behind the greeting
    if (!m_greetingBuffer.empty()) {
        std::string pending;
        pending.swap(m_greetingBuffer);
        handleRequestBytes(reinterpret_cast<const uint8_t*>(pending.data()), pending.size());
    }
}

// The first bytes of the request. Only the command byte is looked at: "VER CMD ...". Everything is
// forwarded unchanged; the command decides which tunnel carries the stream.
void LocalSocksConnection::handleRequestBytes(const uint8_t* data, size_t length)
{
    m_greetingBuffer.append(reinterpret_cast<const char*>(data), length);
    if (m_greetingBuffer.size() < 2)
        return;     // wait for the command byte

    std::string request;
    request.swap(m_greetingBuffer);
    const uint8_t command = static_cast<uint8_t>(request[1]);

    if (command != 0x05) {      // CONNECT and anything else: the server answers
        openStream(reinterpret_cast<const uint8_t*>(request.data()), request.size(), false);
        return;
    }

    // UDP-in-TCP (FWD UDP)
    if (!m_config.udpEnabled) {
        NR_LOG(LOG_DEBUG, "[client] UDP request refused: UDP is disabled");
        rejectRequest(0x07);    // command not supported
        return;
    }

    m_greetingBuffer = std::move(request);      // sent once the UDP tunnel is up
    m_state = State::WaitingUdpTunnel;
    m_waitingForUdpTunnel = true;
    if (!m_pool->requestUdpTunnel(this)) {      // may already have called back
        if (m_waitingForUdpTunnel) {
            m_waitingForUdpTunnel = false;
            NR_LOG(LOG_DEBUG, "[client] UDP tunnel not available, rejecting the flow");
            rejectRequest();
        }
    }
}

void LocalSocksConnection::onUdpTunnelReady()
{
    m_waitingForUdpTunnel = false;
    std::string request;
    request.swap(m_greetingBuffer);
    openStream(reinterpret_cast<const uint8_t*>(request.data()), request.size(), true);
}

void LocalSocksConnection::onUdpTunnelFailed()
{
    m_waitingForUdpTunnel = false;
    NR_LOG(LOG_DEBUG, "[client] UDP tunnel failed, rejecting the flow");
    rejectRequest();
}

void LocalSocksConnection::openStream(const uint8_t* data, size_t length, bool udp)
{
    MultiplexedTunnel* tunnel = nullptr;
    const uint32_t streamId = udp
        ? m_pool->openUdpStream(&LocalSocksConnection::onStreamData, &LocalSocksConnection::onStreamClose, this, tunnel)
        : m_pool->openStream(&LocalSocksConnection::onStreamData, &LocalSocksConnection::onStreamClose, this, tunnel);
    if (streamId == 0) {
        NR_LOG(LOG_DEBUG, "[client] no %stunnel available, rejecting connection", udp ? "UDP " : "");
        rejectRequest();
        return;
    }
    m_udp = udp;

    if (MultiplexedTunnel::Stream* stream = tunnel->getStream(streamId)) {
        stream->onRemoteFin = &LocalSocksConnection::onStreamRemoteFin;
        stream->onPause = &LocalSocksConnection::onStreamPause;
        stream->onResume = &LocalSocksConnection::onStreamResume;
    }

    m_tunnel = tunnel;
    m_streamId = streamId;
    m_state = State::Relaying;
    setIdleTimeout(0);

    NR_LOG(LOG_DEBUG, "[client] stream %u opened%s", streamId, udp ? " (UDP)" : "");

    // the request (and anything sent behind it) travels through the stream unchanged
    tunnel->sendToStream(streamId, data, length);
}

void LocalSocksConnection::rejectRequest(uint8_t code)
{
    uint8_t reply[Socks5Reply::Size];
    Socks5Reply::build(static_cast<Socks5ReplyCode>(code), reply);
    send(reply, sizeof(reply));

    m_state = State::Failed;
    close();
}

// ---- stream callbacks (run on the tunnel's reactor thread, argument is the LocalSocksConnection) ----

void LocalSocksConnection::onStreamData(void* argument, uint32_t /*streamId*/, const uint8_t* data, size_t length)
{
    // the first bytes are the SOCKS5 reply of the server, the rest is data from the target
    LocalSocksConnection* self = static_cast<LocalSocksConnection*>(argument);
    if (self->m_udp)
        self->m_pool->noteUdpActivity();
    self->send(data, length);
}

void LocalSocksConnection::onStreamRemoteFin(void* argument, uint32_t /*streamId*/)
{
    LocalSocksConnection* self = static_cast<LocalSocksConnection*>(argument);
    self->m_streamFinReceived = true;
    self->shutdownWrite();      // FIN to the application once the queue is flushed
}

void LocalSocksConnection::onStreamClose(void* argument, uint32_t streamId)
{
    LocalSocksConnection* self = static_cast<LocalSocksConnection*>(argument);
    NR_LOG(LOG_DEBUG, "[client] stream %u closed", streamId);
    self->m_streamId = 0;
    self->m_tunnel = nullptr;

    // Normal end (FIN received): flush what is queued for the application. RST or tunnel loss: close now.
    self->close(!self->m_streamFinReceived);
}

// The stream's send queue is full: read less from the application.
void LocalSocksConnection::onStreamPause(void* argument, uint32_t /*streamId*/)
{
    static_cast<LocalSocksConnection*>(argument)->pause_reading();
}

void LocalSocksConnection::onStreamResume(void* argument, uint32_t /*streamId*/)
{
    static_cast<LocalSocksConnection*>(argument)->resume_reading();
}
