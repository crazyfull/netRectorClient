#include "clsDNSLookup.h"
#include "clsEventLoop.h"
#include "clsLog.h"

#include <algorithm>
#include <cstring>

#if !defined(_WIN32)
    #include <netdb.h>
#endif

namespace netrector {

namespace {

bool isIpLiteral(const char* host)
{
    unsigned char tmp[sizeof(in6_addr)];
    return inet_pton(AF_INET, host, tmp) == 1 || inet_pton(AF_INET6, host, tmp) == 1;
}

} // namespace

DNSLookup::DNSLookup(EventLoop* loop, int workerThreads)
    : m_loop(loop)
    , m_workerThreads(std::max(1, workerThreads))
    , m_shared(std::make_shared<Shared>())
{
    m_shared->live = loop->liveToken();
}

DNSLookup::~DNSLookup()
{
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->stop = true;
        m_shared->queue.clear();
    }
    m_shared->cv.notify_all();

    // A worker can sit in getaddrinfo() for many seconds when the network is down. The loop must
    // not wait for it: the thread owns a reference to the shared state and ends by itself.
    for (std::thread& t : m_threads)
        t.detach();
}

void DNSLookup::startWorkers()
{
    if (!m_threads.empty())
        return;

    for (int i = 0; i < m_workerThreads; i++)
        m_threads.emplace_back(&DNSLookup::workerMain, m_shared);
}

std::vector<std::string> DNSLookup::lookup(const std::string& host, QUERY_TYPE qtype)
{
    std::vector<std::string> ips;

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = (qtype == A) ? AF_INET : (qtype == AAAA) ? AF_INET6 : AF_UNSPEC;

    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
        return ips;

    for (addrinfo* p = result; p; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        if (p->ai_family == AF_INET) {
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr, buf, sizeof(buf));
        } else if (p->ai_family == AF_INET6) {
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr, buf, sizeof(buf));
        } else {
            continue;
        }

        std::string ip(buf);
        if (std::find(ips.begin(), ips.end(), ip) == ips.end())
            ips.push_back(ip);
    }

    freeaddrinfo(result);
    return ips;
}

void DNSLookup::workerMain(std::shared_ptr<Shared> shared)
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(shared->mutex);
            shared->cv.wait(lock, [&]() { return shared->stop || !shared->queue.empty(); });
            if (shared->stop)
                return;
            job = std::move(shared->queue.front());
            shared->queue.pop_front();
        }

        std::vector<std::string> ips = lookup(job.host, job.qtype);

        // hand the result to the loop thread; dropped if the loop is gone meanwhile
        std::lock_guard<std::mutex> lock(shared->live->mutex);
        EventLoop* loop = shared->live->loop;
        if (!loop)
            continue;

        uint64_t id = job.id;
        loop->post([loop, id, ips]() mutable { loop->dnsResultReady(id, std::move(ips)); });
    }
}

bool DNSLookup::resolve(const char* hostname, callback_t cb, void* user_data, QUERY_TYPE qtype)
{
    if (!hostname || !*hostname || !cb)
        return false;

    if (isIpLiteral(hostname)) {
        // already an address: nothing to resolve
        unsigned char tmp[sizeof(in6_addr)];
        bool isV4 = inet_pton(AF_INET, hostname, tmp) == 1;
        if ((qtype == A && !isV4) || (qtype == AAAA && isV4)) {
            cb(hostname, nullptr, 0, qtype, user_data);
            return true;
        }

        char* ips[1] = { const_cast<char*>(hostname) };
        cb(hostname, ips, 1, qtype, user_data);
        return true;
    }

    startWorkers();

    uint64_t id = m_nextId++;
    m_pending[id] = Pending{cb, user_data, hostname, qtype, m_loop->getCachedNowMs()};

    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->queue.push_back(Job{id, hostname, qtype});
    }
    m_shared->cv.notify_one();
    return true;
}

void DNSLookup::cancel(void* user_data)
{
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->second.userData == user_data)
            it = m_pending.erase(it);
        else
            ++it;
    }
}

void DNSLookup::expire(uint64_t nowMs, uint64_t timeoutMs)
{
    if (m_pending.empty())
        return;

    std::vector<uint64_t> expired;
    for (const auto& kv : m_pending) {
        if (nowMs >= kv.second.startedMs + timeoutMs)
            expired.push_back(kv.first);
    }

    for (uint64_t id : expired)
        deliver(id, std::vector<std::string>());    // the late answer of the worker is ignored
}

// loop thread
void DNSLookup::deliver(uint64_t id, std::vector<std::string> ips)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;     // cancelled

    Pending pending = it->second;
    m_pending.erase(it);

    std::vector<char*> raw;
    raw.reserve(ips.size());
    for (std::string& ip : ips)
        raw.push_back(&ip[0]);

    pending.cb(pending.host.c_str(), raw.empty() ? nullptr : raw.data(), raw.size(), pending.qtype, pending.userData);
}

} // namespace netrector
