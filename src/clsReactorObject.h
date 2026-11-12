#ifndef CLSREACTOROBJECT_H
#define CLSREACTOROBJECT_H

// Base of every object the loop owns and deletes at the end of an iteration (deleteLater):
// TCPSocket, UDPSocket, ...
#include <cstdint>
#include <mutex>

namespace netrector {

class ReactorObject {
public:
    virtual ~ReactorObject() = default;
};

// Readiness bits: both the interest of an object and the events the loop reports back.
// EV_ERROR and EV_HUP are always reported, they cannot be requested.
enum IoEvent : uint32_t {
    EV_READ  = 1,
    EV_WRITE = 2,
    EV_ERROR = 4,
    EV_HUP   = 8
};

// What the loop polls. Implemented by TCPSocket, UDPSocket and Listener.
class IoObject {
public:
    virtual ~IoObject() = default;

    // readiness (IoEvent bits)
    virtual void onIoEvents(uint32_t events) = 0;

    // work scheduled with EventLoop::defer(), runs at the end of the loop iteration
    virtual void onDeferred(int /*kind*/) {}

    // about once per second: connect/idle/linger timeouts, keep-alive
    virtual void onMaintenance(uint64_t /*nowSeconds*/) {}

    // the loop is shutting down (EventLoop::closeAll): close now
    virtual void onLoopShutdown() {}
};

class EventLoop;
// Liveness token for other threads (DNS workers): checked under the mutex before post()
struct ReactorLiveToken {
    std::mutex mutex;
    EventLoop* loop { nullptr };
};

} // namespace netrector

#endif // CLSREACTOROBJECT_H
