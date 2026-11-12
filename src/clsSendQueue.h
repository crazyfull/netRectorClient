#ifndef CLSSENDQUEUE_H
#define CLSSENDQUEUE_H

#include "clsPlatform.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

// ============================== SendQueue ====================================
// Outgoing data of a socket as a chain of chunks (intrusive list, no std container).
// An empty SendQueue owns no heap memory, which matters for idle connections.
//
// Chunk kinds:
//   Heap     : copied data (push / pushv)
//   External : pointer to application memory, no copy (pushExternal); released after sending
namespace netrector {

class SendQueue {
public:
    enum ChunkKind : uint8_t { Heap = 0, External = 1 };
    using ReleaseFn = void(*)(void* data, void* arg);

    struct Chunk {
        Chunk* next;
        size_t len;         // Heap: bytes written | External: total length
        size_t off;         // bytes already sent
        size_t cap;         // Heap: capacity of the data area
        ChunkKind kind;
        const char* extPtr;
        ReleaseFn extRelease;
        void* extArg;

        // pointer to the unsent data
        const char* unsent() const {
            const char* base = (kind == External) ? extPtr : reinterpret_cast<const char*>(this + 1);
            return base + off;
        }
        size_t avail() const { return len - off; }
        char* writable() { return reinterpret_cast<char*>(this + 1); }
    };

    SendQueue() = default;
    ~SendQueue();
    SendQueue(const SendQueue&) = delete;
    SendQueue& operator=(const SendQueue&) = delete;

    // transactional: either all of the data is queued or nothing (false: out of memory)
    bool push(const void* data, size_t len);
    bool pushv(const struct iovec* iov, int iovcnt);
    bool pushExternal(const void* data, size_t len, size_t alreadySent, ReleaseFn release, void* arg);

    bool empty() const { return m_head == nullptr; }
    Chunk* front() { return m_head; }
    const Chunk* front() const { return m_head; }
    void pop_front();
    void consume(size_t n);     // n bytes were sent
    void clear();
    size_t size() const { return m_len; }       // unsent bytes
    size_t count() const { return m_count; }

private:
    Chunk* m_head { nullptr };
    Chunk* m_tail { nullptr };
    size_t m_len { 0 };
    size_t m_count { 0 };

    Chunk* allocChunk(size_t dataCap);
    void freeChunk(Chunk* c);
    void append(Chunk* c);
};

} // namespace netrector

#endif // CLSSENDQUEUE_H
