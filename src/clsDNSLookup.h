#ifndef CLSDNSLOOKUP_H
#define CLSDNSLOOKUP_H
// ============================== DNSLookup ====================================
// Name resolution through the system resolver (getaddrinfo) on a small worker thread pool, so it
// works the same on every platform (netRector's own UDP DNS client reads /etc/resolv.conf, which
// does not exist on Windows, Android or iOS). The result is delivered on the loop thread.
#include "clsPlatform.h"
#include "clsReactorObject.h"
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace netrector {

class EventLoop;

class DNSLookup {
public:
    enum QUERY_TYPE {
        Any = 0,        // IPv4 and IPv6 addresses, in the order the system prefers
        A = 1,
        AAAA = 28
    };

    // ips are valid only until the callback returns. count == 0: not resolved.
    typedef void (*callback_t)(const char* hostname, char** ips, size_t count, DNSLookup::QUERY_TYPE qtype, void* user_data);

    explicit DNSLookup(EventLoop* loop, int workerThreads = 2);
    ~DNSLookup();

    // true: the callback is called exactly once, possibly before resolve() returns (the host is an
    //       IP literal). false: not accepted, no callback.
    bool resolve(const char* hostname, callback_t cb, void* user_data, DNSLookup::QUERY_TYPE qtype = Any);

    // drops every pending request with this user_data (its callback is not called)
    void cancel(void* user_data);

    // internal: requests older than timeoutMs are answered with "not resolved" (loop maintenance)
    void expire(uint64_t nowMs, uint64_t timeoutMs);

    // internal: the loop thread hands over the result of a worker (EventLoop::dnsResultReady)
    void deliver(uint64_t id, std::vector<std::string> ips);

private:
    struct Job {
        uint64_t id;
        std::string host;
        QUERY_TYPE qtype;
    };

    struct Pending {
        callback_t cb;
        void* userData;
        std::string host;
        QUERY_TYPE qtype;
        uint64_t startedMs;
    };

    // shared with the worker threads, which may outlive this object for a short time
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Job> queue;
        bool stop { false };
        std::shared_ptr<ReactorLiveToken> live;
    };

    static void workerMain(std::shared_ptr<Shared> shared);
    static std::vector<std::string> lookup(const std::string& host, QUERY_TYPE qtype);
    void startWorkers();

    EventLoop* m_loop;
    int m_workerThreads;
    std::shared_ptr<Shared> m_shared;
    std::vector<std::thread> m_threads;
    std::unordered_map<uint64_t, Pending> m_pending;
    uint64_t m_nextId { 1 };
};

} // namespace netrector

#endif // CLSDNSLOOKUP_H
