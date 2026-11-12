#include "clsEventLoop.h"
#include "clsLog.h"
#include "clsTCPSocket.h"

#include <algorithm>
#include <cstring>

namespace netrector {

// ============================== WakeUp =======================================
// UDP socket bound to 127.0.0.1 and connected to itself: any thread can send one byte to wake
// the loop out of its wait. Portable (no pipe/eventfd/socketpair needed, works with select()).
class EventLoop::WakeUp : public IoObject {
public:
    ~WakeUp() { close(); }

    bool open(EventLoop* loop)
    {
        m_loop = loop;
        m_fd = platform::createSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_fd == NR_INVALID_SOCKET)
            return false;

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        socklen_t len = sizeof(addr);
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
            || ::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0
            || ::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }

        m_token = loop->addObject(this, m_fd, EV_READ, KindWake);
        if (m_token == 0) {
            close();
            return false;
        }
        return true;
    }

    void close()
    {
        if (m_token && m_loop) {
            m_loop->removeObject(m_token);
            m_token = 0;
        }
        if (m_fd != NR_INVALID_SOCKET) {
            platform::closeSocket(m_fd);
            m_fd = NR_INVALID_SOCKET;
        }
    }

    // any thread
    void signal()
    {
        static const char byte = 'w';
        if (m_fd != NR_INVALID_SOCKET)
            platform::sendData(m_fd, &byte, 1);     // a full buffer means a wake-up is pending anyway
    }

    void onIoEvents(uint32_t) override
    {
        char buf[64];
        for (int i = 0; i < 32; i++) {
            ssize_t n = platform::recvData(m_fd, buf, sizeof(buf));
            if (n <= 0)
                break;
        }
    }

private:
    EventLoop* m_loop { nullptr };
    NrSocket m_fd { NR_INVALID_SOCKET };
    uint64_t m_token { 0 };
};

// ============================== EventLoop ====================================

EventLoop::EventLoop(int id)
    : m_loopId(id)
    , m_readBuffer(READ_BUFFER_SIZE)
    , m_scratchBuffer(READ_BUFFER_SIZE + 64)
    , m_liveToken(std::make_shared<ReactorLiveToken>())
{
    m_platformReady = platform::init();
    m_liveToken->loop = this;
    updateCashedTime();
    m_lastMaintenanceMs = m_nowMs;

    m_wakeUp.reset(new WakeUp);
    if (!m_wakeUp->open(this)) {
        NR_LOG(LOG_ERROR, "EventLoop: cannot create the wake-up socket (%s)", errorString(platform::lastError()).c_str());
        m_wakeUp.reset();
    }
}

EventLoop::~EventLoop()
{
    closeAll();

    {
        // from now on the DNS workers cannot post to this loop any more
        std::lock_guard<std::mutex> lock(m_liveToken->mutex);
        m_liveToken->loop = nullptr;
    }

    m_dns.reset();
    m_wakeUp.reset();

    if (m_platformReady)
        platform::shutdown();
}

void EventLoop::updateCashedTime()
{
    m_nowMs = platform::monotonicMs();
}

void EventLoop::wake()
{
    if (m_wakeUp)
        m_wakeUp->signal();
}

void EventLoop::post(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_tasks.push_back(std::move(task));
    }
    m_hasTasks.store(true, std::memory_order_release);
    wake();
}

void EventLoop::stop()
{
    m_stopRequested.store(true);
    wake();
}

uint64_t EventLoop::addObject(IoObject* obj, NrSocket fd, uint32_t interest, IoKind kind)
{
    if (kind == KindTcp && m_maxConnections != 0 && m_tcpCount >= m_maxConnections)
        return 0;       // connection limit reached

    uint64_t token = m_nextToken++;
    if (!m_poller.add(token, fd, interest))
        return 0;

    m_objects[token] = Registered{obj, kind};
    if (kind == KindTcp)
        m_tcpCount++;
    return token;
}

void EventLoop::setInterest(uint64_t token, uint32_t interest)
{
    m_poller.modify(token, interest);
}

void EventLoop::removeObject(uint64_t token)
{
    auto it = m_objects.find(token);
    if (it == m_objects.end())
        return;

    if (it->second.kind == KindTcp && m_tcpCount > 0)
        m_tcpCount--;

    m_poller.remove(token);
    m_objects.erase(it);
}

void EventLoop::defer(uint64_t token, int kind)
{
    m_deferred.push_back(DeferredItem{token, kind});
}

void EventLoop::deleteLater(ReactorObject* obj)
{
    if (obj)
        m_garbage.push_back(obj);
}

bool EventLoop::getIPbyName(const char* hostname, DNSLookup::callback_t callback, void* p, DNSLookup::QUERY_TYPE type)
{
    if (!m_dns)
        m_dns.reset(new DNSLookup(this));
    return m_dns->resolve(hostname, callback, p, type);
}

void EventLoop::cancelDNS(void* p)
{
    if (m_dns)
        m_dns->cancel(p);
}

void EventLoop::dnsResultReady(uint64_t id, std::vector<std::string> ips)
{
    if (m_dns)
        m_dns->deliver(id, std::move(ips));
}

// ---------------------------------------------------------------------------

void EventLoop::runPostedTasks()
{
    if (!m_hasTasks.exchange(false, std::memory_order_acq_rel))
        return;

    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        tasks.swap(m_tasks);
    }

    for (auto& task : tasks) {
        if (task)
            task();
    }
}

void EventLoop::runDeferred()
{
    if (m_deferred.empty())
        return;

    // work added while running belongs to the next iteration
    m_deferredRunning.swap(m_deferred);

    for (const DeferredItem& item : m_deferredRunning) {
        auto it = m_objects.find(item.token);
        if (it == m_objects.end())
            continue;   // closed in the meantime
        it->second.obj->onDeferred(item.kind);
    }

    m_deferredRunning.clear();
}

// Objects closed during an iteration are deleted only here, after every callback of the
// iteration has returned, so no callback can run with a pointer to a deleted object.
void EventLoop::collectGarbage()
{
    while (!m_garbage.empty()) {
        std::vector<ReactorObject*> dead;
        dead.swap(m_garbage);
        for (ReactorObject* obj : dead)
            delete obj;
    }
}

// connect/handshake/idle/linger timeouts, mux keep-alive: once per MAINTENANCE_INTERVAL_MS
void EventLoop::maintenance()
{
    m_lastMaintenanceMs = m_nowMs;
    const uint64_t nowSeconds = m_nowMs / 1000;

    if (m_dns)
        m_dns->expire(m_nowMs, (uint64_t)CONNECT_TIMEOUT_SECS * 1000);

    std::vector<uint64_t> tokens;
    tokens.reserve(m_objects.size());
    for (const auto& kv : m_objects) {
        if (kv.second.kind != KindWake)
            tokens.push_back(kv.first);
    }

    for (uint64_t token : tokens) {
        auto it = m_objects.find(token);
        if (it != m_objects.end())
            it->second.obj->onMaintenance(nowSeconds);
    }
}

int EventLoop::runOnce(int timeoutMs)
{
    updateCashedTime();

    int wait = timeoutMs;
    if (!m_deferred.empty() || m_hasTasks.load(std::memory_order_acquire)) {
        wait = 0;
    } else {
        uint64_t elapsed = m_nowMs - m_lastMaintenanceMs;
        int dueIn = elapsed >= (uint64_t)MAINTENANCE_INTERVAL_MS ? 0 : (int)(MAINTENANCE_INTERVAL_MS - elapsed);
        if (wait < 0 || wait > dueIn)
            wait = dueIn;
    }

    m_results.clear();
    int n = m_poller.wait(wait, m_results);
    if (n < 0) {
        NR_LOG(LOG_ERROR, "EventLoop: wait failed (%s)", errorString(platform::lastError()).c_str());
        return -1;
    }

    updateCashedTime();

    int handled = 0;
    for (size_t i = 0; i < m_results.size(); i++) {
        const PollResult r = m_results[i];
        auto it = m_objects.find(r.token);
        if (it == m_objects.end())
            continue;   // closed by an earlier callback of this iteration

        if (it->second.kind != KindWake)
            handled++;
        it->second.obj->onIoEvents(r.events);
    }

    runPostedTasks();
    runDeferred();

    if (m_nowMs - m_lastMaintenanceMs >= (uint64_t)MAINTENANCE_INTERVAL_MS)
        maintenance();

    collectGarbage();
    return handled;
}

void EventLoop::run(std::atomic<bool>& stop)
{
    while (!stop.load(std::memory_order_relaxed) && !m_stopRequested.load(std::memory_order_relaxed)) {
        if (runOnce(MAINTENANCE_INTERVAL_MS) < 0)
            break;
    }

    m_stopRequested.store(false);
    closeAll();
}

void EventLoop::run()
{
    std::atomic<bool> never { false };
    run(never);
}

void EventLoop::closeAll()
{
    if (m_closing)
        return;
    m_closing = true;

    std::vector<uint64_t> tokens;
    for (const auto& kv : m_objects) {
        if (kv.second.kind != KindWake)
            tokens.push_back(kv.first);
    }

    for (uint64_t token : tokens) {
        auto it = m_objects.find(token);
        if (it != m_objects.end())
            it->second.obj->onLoopShutdown();
    }

    m_deferred.clear();
    collectGarbage();
    m_closing = false;
}

} // namespace netrector
