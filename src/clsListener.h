#ifndef LISTENER_H
#define LISTENER_H

#include "clsEventLoop.h"

// ============================== Listener =====================================
// A TCP listener on one EventLoop that can be started and stopped separately
// (the same class as in netRector; there is no Server/shard here: one Listener per loop).
//
//   Listener* l = new Listener(&loop);
//   l->start(8080, "0.0.0.0", myAcceptFactory, ctx);
//   ...
//   l->stop();  delete l;
//
// The factory returns a new TCPSocket for every connection (or nullptr to refuse it). The loop
// owns the socket from then on. Stop the listener (or delete it) before the loop is destroyed;
// EventLoop::closeAll() also stops it.
namespace netrector {

class Listener : public IoObject
{
public:
    explicit Listener(EventLoop* loop = nullptr);
    ~Listener() override;

    void setReactor(EventLoop* loop) { m_pReactor = loop; }

    // bindIP: nullptr/"*" = all interfaces (IPv4+IPv6), "0.0.0.0" = all IPv4, or one address
    bool start(int port, const char* bindIP, EventLoop::acceptCallback cb, void* ctx);
    void stop();

    bool isListening() const { return m_fd != NR_INVALID_SOCKET; }
    int fd() const { return m_fd == NR_INVALID_SOCKET ? -1 : (int)m_fd; }
    int port() const { return m_port; }
    int lastError() const { return m_lastError; }

    // legacy
    bool Addlisten(int port) { return start(port, nullptr, nullptr, nullptr); }

    // internal (IoObject)
    void onIoEvents(uint32_t ev) override;
    void onMaintenance(uint64_t nowSeconds) override;
    void onLoopShutdown() override { stop(); }

private:
    EventLoop* m_pReactor = nullptr;
    NrSocket m_fd = NR_INVALID_SOCKET;
    uint64_t m_token = 0;
    int m_port = 0;
    int m_lastError = 0;
    bool m_paused = false;
    uint64_t m_pausedAt = 0;
    EventLoop::acceptCallback m_factory = nullptr;
    void* m_ctx = nullptr;

    void acceptLoop();
};

} // namespace netrector

#endif // LISTENER_H
