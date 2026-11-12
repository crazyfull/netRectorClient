#ifndef CLSPOLLER_H
#define CLSPOLLER_H
// ============================== Poller (internal) ============================
// Readiness notification behind EventLoop. The only place that knows how to wait for sockets:
//   POSIX   (Linux, Android, iOS, macOS): poll()
//   Windows                             : select() (WSAPoll misreports failed non-blocking connects)
// Level-triggered. Objects are identified by a token chosen by the loop, never by the descriptor,
// so a descriptor number that is reused inside one loop iteration cannot receive a stale event.
#include "clsPlatform.h"
#include "clsReactorObject.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace netrector {

struct PollResult {
    uint64_t token;
    uint32_t events;    // IoEvent bits
};

class Poller {
public:
    Poller();
    ~Poller();

    bool add(uint64_t token, NrSocket fd, uint32_t interest);
    void modify(uint64_t token, uint32_t interest);
    void remove(uint64_t token);
    size_t size() const { return m_entries.size(); }

    // Waits up to timeoutMs (-1 = forever, 0 = do not wait) and appends the ready objects to out.
    // Returns the number of ready objects, or -1 on a fatal error. Signals (EINTR) return 0.
    int wait(int timeoutMs, std::vector<PollResult>& out);

private:
    struct Entry {
        uint64_t token;
        NrSocket fd;
        uint32_t interest;
    };

    std::vector<Entry> m_entries;
    std::unordered_map<uint64_t, size_t> m_index;   // token -> position in m_entries (and m_pollfds)

#if !defined(_WIN32)
    std::vector<struct pollfd> m_pollfds;           // parallel to m_entries
    void applyInterest(size_t i);
#else
    std::vector<char> m_readBuf, m_writeBuf, m_errorBuf;   // fd_set storage, grown on demand
    std::unordered_map<NrSocket, uint64_t> m_fdToToken;
#endif
};

} // namespace netrector

#endif // CLSPOLLER_H
