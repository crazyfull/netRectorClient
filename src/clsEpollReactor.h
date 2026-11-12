#ifndef CLSEPOLLREACTOR_H
#define CLSEPOLLREACTOR_H
// Source compatibility with netRector: code written against netRector's EpollReactor builds
// against netRectorClient by only changing the include path. The class is the portable EventLoop.
// Not available here: shards, Server, addTimer/removeTimer, epoll specific tuning.
#include "clsEventLoop.h"

namespace netrector {
using EpollReactor = EventLoop;
} // namespace netrector

#endif // CLSEPOLLREACTOR_H
