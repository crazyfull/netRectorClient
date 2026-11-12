#include "clsPoller.h"

#include <cstring>

namespace netrector {

Poller::Poller() {}
Poller::~Poller() {}

#if !defined(_WIN32)
// ============================== POSIX: poll() ================================

void Poller::applyInterest(size_t i)
{
    struct pollfd& p = m_pollfds[i];
    const Entry& e = m_entries[i];

    p.events = 0;
    if (e.interest & EV_READ)
        p.events |= POLLIN;
    if (e.interest & EV_WRITE)
        p.events |= POLLOUT;

    // poll() reports POLLHUP/POLLERR even when nothing is requested. A descriptor nobody is
    // interested in (e.g. a socket whose reading is paused) is switched off completely with a
    // negative fd, so a half-closed peer cannot make the loop spin.
    p.fd = (e.interest == 0) ? (int)(-e.fd - 1) : e.fd;
    p.revents = 0;
}

bool Poller::add(uint64_t token, NrSocket fd, uint32_t interest)
{
    if (m_index.count(token))
        return false;

    size_t i = m_entries.size();
    m_entries.push_back(Entry{token, fd, interest});
    m_pollfds.push_back(pollfd{});
    m_index[token] = i;
    applyInterest(i);
    return true;
}

void Poller::modify(uint64_t token, uint32_t interest)
{
    auto it = m_index.find(token);
    if (it == m_index.end())
        return;

    m_entries[it->second].interest = interest;
    applyInterest(it->second);
}

void Poller::remove(uint64_t token)
{
    auto it = m_index.find(token);
    if (it == m_index.end())
        return;

    size_t i = it->second;
    size_t last = m_entries.size() - 1;
    if (i != last) {
        m_entries[i] = m_entries[last];
        m_pollfds[i] = m_pollfds[last];
        m_index[m_entries[i].token] = i;
    }
    m_entries.pop_back();
    m_pollfds.pop_back();
    m_index.erase(it);
}

int Poller::wait(int timeoutMs, std::vector<PollResult>& out)
{
    int n = ::poll(m_pollfds.data(), (nfds_t)m_pollfds.size(), timeoutMs);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    if (n == 0)
        return 0;

    int found = 0;
    for (size_t i = 0; i < m_pollfds.size() && found < n; i++) {
        short r = m_pollfds[i].revents;
        if (r == 0)
            continue;
        found++;

        uint32_t ev = 0;
        if (r & POLLIN)
            ev |= EV_READ;
        if (r & POLLOUT)
            ev |= EV_WRITE;
        if (r & (POLLERR | POLLNVAL))
            ev |= EV_ERROR;
        if (r & POLLHUP)
            ev |= EV_HUP;

        m_pollfds[i].revents = 0;
        out.push_back(PollResult{m_entries[i].token, ev});
    }
    return found;
}

#else
// ============================== Windows: select() ============================
// fd_set is a counted array on Winsock, so a set of any size can be laid out by hand.
namespace {

struct WinFdSet {
    u_int count;
    SOCKET array[1];
};

size_t setBytes(size_t capacity)
{
    return sizeof(u_int) + (capacity ? capacity : 1) * sizeof(SOCKET) + sizeof(SOCKET);
}

inline WinFdSet* asSet(std::vector<char>& buf, size_t capacity)
{
    size_t bytes = setBytes(capacity);
    if (buf.size() < bytes)
        buf.resize(bytes);
    WinFdSet* s = reinterpret_cast<WinFdSet*>(buf.data());
    s->count = 0;
    return s;
}

inline void addToSet(WinFdSet* s, SOCKET fd) { s->array[s->count++] = fd; }

} // namespace

bool Poller::add(uint64_t token, NrSocket fd, uint32_t interest)
{
    if (m_index.count(token))
        return false;

    m_index[token] = m_entries.size();
    m_entries.push_back(Entry{token, fd, interest});
    m_fdToToken[fd] = token;
    return true;
}

void Poller::modify(uint64_t token, uint32_t interest)
{
    auto it = m_index.find(token);
    if (it != m_index.end())
        m_entries[it->second].interest = interest;
}

void Poller::remove(uint64_t token)
{
    auto it = m_index.find(token);
    if (it == m_index.end())
        return;

    size_t i = it->second;
    m_fdToToken.erase(m_entries[i].fd);
    size_t last = m_entries.size() - 1;
    if (i != last) {
        m_entries[i] = m_entries[last];
        m_index[m_entries[i].token] = i;
    }
    m_entries.pop_back();
    m_index.erase(it);
}

int Poller::wait(int timeoutMs, std::vector<PollResult>& out)
{
    size_t cap = m_entries.size();
    WinFdSet* rd = asSet(m_readBuf, cap);
    WinFdSet* wr = asSet(m_writeBuf, cap);
    WinFdSet* ex = asSet(m_errorBuf, cap);

    for (const Entry& e : m_entries) {
        if (e.interest == 0)
            continue;
        if (e.interest & EV_READ)
            addToSet(rd, e.fd);
        if (e.interest & EV_WRITE)
            addToSet(wr, e.fd);
        addToSet(ex, e.fd);     // failed non-blocking connect() is reported here
    }

    if (rd->count == 0 && wr->count == 0 && ex->count == 0) {
        if (timeoutMs > 0)
            Sleep((DWORD)timeoutMs);
        return 0;
    }

    timeval tv;
    timeval* ptv = nullptr;
    if (timeoutMs >= 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ptv = &tv;
    }

    int n = ::select(0, reinterpret_cast<fd_set*>(rd), reinterpret_cast<fd_set*>(wr), reinterpret_cast<fd_set*>(ex), ptv);
    if (n == SOCKET_ERROR)
        return WSAGetLastError() == WSAEINTR ? 0 : -1;
    if (n == 0)
        return 0;

    // map the ready descriptors back to tokens (only the ready ones are visited)
    std::unordered_map<SOCKET, uint32_t> ready;
    for (u_int i = 0; i < rd->count; i++) ready[rd->array[i]] |= EV_READ;
    for (u_int i = 0; i < wr->count; i++) ready[wr->array[i]] |= EV_WRITE;
    for (u_int i = 0; i < ex->count; i++) ready[ex->array[i]] |= EV_ERROR;

    int found = 0;
    for (const auto& kv : ready) {
        auto it = m_fdToToken.find(kv.first);
        if (it == m_fdToToken.end())
            continue;
        out.push_back(PollResult{it->second, kv.second});
        found++;
    }
    return found;
}

#endif

} // namespace netrector
