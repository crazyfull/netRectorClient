#ifndef CLSEVENTLOOP_H
#define CLSEVENTLOOP_H
// ============================== EventLoop ====================================
// The one object that waits for socket events and runs callbacks. It replaces netRector's
// EpollReactor/Server/shard: one loop = one thread. Use one EventLoop per thread when you need
// more; nothing is shared between loops.
//
//   EventLoop loop;
//   TCPSocket* s = new MySocket; s->setReactor(&loop); s->connectTo("example.com", 80);
//   std::atomic<bool> stop{false};
//   loop.run(stop);                       // or: while (...) loop.runOnce(100);
//
// All methods except post(), wake() and stop() must be called from the thread that runs the loop
// (use post() to get there from another thread). There are no user timers: the loop wakes up about
// once per second for internal timeouts, and an application that needs a clock drives the loop
// with runOnce() and checks its own time after each call.
#include "clsDNSLookup.h"
#include "clsPlatform.h"
#include "clsPoller.h"
#include "clsReactorObject.h"
#include "constants.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace netrector {

class TCPSocket;

class EventLoop
{
public:
    explicit EventLoop(int id = 0);
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // factory of accepted connections: return a new TCPSocket, or nullptr to refuse the connection
    using acceptCallback = TCPSocket* (*)(void*);

    // Runs until stop becomes true (check is made at least once per second, and after every wake()).
    // All sockets are closed with close(true) before it returns (their onClose callbacks run).
    void run(std::atomic<bool>& stop);

    // Runs until stop() is called.
    void run();
    void stop();                    // thread-safe

    // One iteration: waits up to timeoutMs for events (0 = do not wait, -1 = until something
    // happens), dispatches them, runs posted tasks and the once-per-second maintenance.
    // Returns the number of I/O events handled. Use it to embed the loop in an application loop.
    int runOnce(int timeoutMs);

    // Closes every socket now (close(true), onClose is called) and deletes them. Called by run()
    // when it ends and by the destructor.
    void closeAll();

    // thread-safe: wake the loop / run a function on the loop thread
    void wake();
    void post(std::function<void()> task);

    // Default factory for listeners without their own factory.
    void setOnAccepted(acceptCallback fn, void* ctx) { m_onAccepted = fn; m_onAcceptedCtx = ctx; }
    acceptCallback onAcceptedFactory() const { return m_onAccepted; }
    void* onAcceptedContext() const { return m_onAcceptedCtx; }

    // DNS (system resolver on worker threads). The callback may run before this returns when
    // hostname is an IP literal. cancelDNS drops the pending requests of one user pointer.
    bool getIPbyName(const char* hostname, DNSLookup::callback_t callback, void* p, DNSLookup::QUERY_TYPE type = DNSLookup::Any);
    void cancelDNS(void* p);

    void deleteLater(ReactorObject* obj);       // deleted at the end of the current iteration

    // seconds / milliseconds on a monotonic clock, refreshed once per iteration
    uint64_t getCachedNow() const { return m_nowMs / 1000; }
    uint64_t getCachedNowMs() const { return m_nowMs; }
    void updateCashedTime();

    int id() const { return m_loopId; }
    uint32_t connectionCount() const { return m_tcpCount; }
    uint32_t tcpConnectionCount() const { return m_tcpCount; }

    // saghf of TCP connections (accepted + outgoing) on this loop; 0 = no limit
    void setMaxConnections(uint32_t n) { m_maxConnections = n; }
    uint32_t maxConnections() const { return m_maxConnections; }

    // one pointer for the application (e.g. state shared by the connections of this loop)
    void setUserData(void* p) { m_userData = p; }
    void* userData() const { return m_userData; }

    std::shared_ptr<ReactorLiveToken> liveToken() const { return m_liveToken; }

    // read buffer shared by all sockets of the loop: its content is valid only until the callback returns
    char* readBuffer() { return m_readBuffer.data(); }
    size_t readBufferSize() const { return m_readBuffer.size(); }

    // temporary buffer for gathering writes (TLS)
    char* scratchBuffer() { return m_scratchBuffer.data(); }
    size_t scratchBufferSize() const { return m_scratchBuffer.size(); }

    // ---- internal: used by TCPSocket, UDPSocket and Listener ----
    enum IoKind { KindTcp = 1, KindUdp = 2, KindListener = 3, KindWake = 4 };

    // registers fd; returns the token (0 = failed)
    uint64_t addObject(IoObject* obj, NrSocket fd, uint32_t interest, IoKind kind);
    void setInterest(uint64_t token, uint32_t interest);
    void removeObject(uint64_t token);

    // run obj->onDeferred(kind) at the end of the iteration (skipped if the object is gone by then)
    void defer(uint64_t token, int kind);

    // result of a DNS worker (loop thread)
    void dnsResultReady(uint64_t id, std::vector<std::string> ips);

private:
    struct Registered {
        IoObject* obj;
        IoKind kind;
    };

    struct DeferredItem {
        uint64_t token;
        int kind;
    };

    int m_loopId;
    uint64_t m_nowMs { 0 };
    uint64_t m_lastMaintenanceMs { 0 };
    uint64_t m_nextToken { 1 };
    uint32_t m_tcpCount { 0 };
    uint32_t m_maxConnections { 0 };
    void* m_userData { nullptr };
    bool m_platformReady { false };
    bool m_closing { false };

    Poller m_poller;
    std::unordered_map<uint64_t, Registered> m_objects;
    std::vector<PollResult> m_results;
    std::vector<DeferredItem> m_deferred;
    std::vector<DeferredItem> m_deferredRunning;
    std::vector<ReactorObject*> m_garbage;

    std::mutex m_taskMutex;
    std::vector<std::function<void()>> m_tasks;
    std::atomic<bool> m_hasTasks { false };
    std::atomic<bool> m_stopRequested { false };

    std::vector<char> m_readBuffer;
    std::vector<char> m_scratchBuffer;

    acceptCallback m_onAccepted { nullptr };
    void* m_onAcceptedCtx { nullptr };

    std::shared_ptr<ReactorLiveToken> m_liveToken;
    std::unique_ptr<DNSLookup> m_dns;

    // wake-up channel: a UDP socket bound to loopback that sends to itself (works on every platform)
    class WakeUp;
    std::unique_ptr<WakeUp> m_wakeUp;

    void runPostedTasks();
    void runDeferred();
    void collectGarbage();
    void maintenance();
};

} // namespace netrector

#endif // CLSEVENTLOOP_H
