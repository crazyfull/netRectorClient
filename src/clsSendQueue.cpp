#include "clsSendQueue.h"
#include "constants.h"
#include <algorithm>
#include <cstdlib>

namespace netrector {

SendQueue::~SendQueue() {
    clear();
}

SendQueue::Chunk* SendQueue::allocChunk(size_t dataCap)
{
    Chunk* c = static_cast<Chunk*>(malloc(sizeof(Chunk) + dataCap));
    if (!c)
        return nullptr;

    c->next = nullptr;
    c->len = 0;
    c->off = 0;
    c->cap = dataCap;
    c->kind = Heap;
    c->extPtr = nullptr;
    c->extRelease = nullptr;
    c->extArg = nullptr;
    return c;
}

void SendQueue::freeChunk(Chunk* c)
{
    if (c->kind == External && c->extRelease)
        c->extRelease(const_cast<char*>(c->extPtr), c->extArg);
    free(c);
}

void SendQueue::append(Chunk* c)
{
    if (m_tail)
        m_tail->next = c;
    else
        m_head = c;
    m_tail = c;
    m_count++;
}

bool SendQueue::push(const void* data, size_t len)
{
    struct iovec iov;
    iov.iov_base = const_cast<void*>(data);
    iov.iov_len = len;
    return pushv(&iov, 1);
}

bool SendQueue::pushv(const struct iovec* iov, int iovcnt)
{
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++)
        total += iov[i].iov_len;
    if (total == 0)
        return true;

    // free room in the last block (only if it is a copy block)
    size_t room = 0;
    if (m_tail && m_tail->kind == Heap)
        room = m_tail->cap - m_tail->len;

    // allocate first so that nothing changes on failure
    Chunk* fresh = nullptr;
    if (total > room) {
        size_t need = total - room;
        fresh = allocChunk(std::max(need, SEND_BLOCK_MIN));
        if (!fresh)
            return false;
    }

    size_t remainingRoom = room;
    for (int i = 0; i < iovcnt; i++) {
        const char* src = static_cast<const char*>(iov[i].iov_base);
        size_t n = iov[i].iov_len;

        if (remainingRoom > 0 && n > 0) {
            size_t c = std::min(remainingRoom, n);
            memcpy(m_tail->writable() + m_tail->len, src, c);
            m_tail->len += c;
            remainingRoom -= c;
            src += c;
            n -= c;
        }

        if (n > 0) {
            memcpy(fresh->writable() + fresh->len, src, n);
            fresh->len += n;
        }
    }

    if (fresh)
        append(fresh);

    m_len += total;
    return true;
}

bool SendQueue::pushExternal(const void* data, size_t len, size_t alreadySent, ReleaseFn release, void* arg)
{
    Chunk* c = allocChunk(0);
    if (!c)
        return false;

    c->len = len;
    c->off = alreadySent;
    c->kind = External;
    c->extPtr = static_cast<const char*>(data);
    c->extRelease = release;
    c->extArg = arg;
    append(c);
    m_len += len - alreadySent;
    return true;
}

void SendQueue::pop_front() {
    Chunk* c = m_head;
    if (!c)
        return;

    m_len -= c->avail();
    m_head = c->next;
    if (!m_head)
        m_tail = nullptr;
    m_count--;
    freeChunk(c);
}

void SendQueue::consume(size_t n) {
    while (n > 0 && m_head) {
        Chunk* c = m_head;
        size_t avail = c->avail();
        if (n >= avail) {
            n -= avail;
            pop_front();
        } else {
            c->off += n;
            m_len -= n;
            n = 0;
        }
    }
}

void SendQueue::clear() {
    while (m_head)
        pop_front();
}

} // namespace netrector
