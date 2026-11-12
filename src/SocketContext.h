#ifndef SOCKETCONTEXT_H
#define SOCKETCONTEXT_H
// ============================== SocketContext (low-level) ====================
// Owns the descriptor, the loop registration and the send queue of a socket; used by composition
// by TCPSocket and UDPSocket. There is no per-socket read buffer: every loop has one shared
// buffer (EventLoop::readBuffer) because data is consumed inside the callback.
#include "clsPlatform.h"
#include "clsSendQueue.h"
#include <cstdint>

namespace netrector {

struct SocketContext
{
    NrSocket fd { NR_INVALID_SOCKET };
    uint16_t port { 0 };
    uint64_t token { 0 };      // registration in the EventLoop (0 = not registered)
    SendQueue writeQueue;      // chain of chunks; empty = no heap memory
    uint64_t lastActive {};
};

} // namespace netrector

#endif // SOCKETCONTEXT_H
