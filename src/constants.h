#ifndef CONSTANTS_H
#define CONSTANTS_H
#include <cstddef>

// Socket and loop constants (a subset of netRector's constants.h: no epoll, pool or timer values)
namespace netrector {

static constexpr int MAX_ACCEPT_PER_EVENT = 64;     // fairness: max accept per listener event
static constexpr int MAX_READS_PER_EVENT = 16;      // fairness: max recv per event of one socket
static constexpr int MAX_UDP_PER_EVENT = 64;        // fairness: max datagrams per event of one UDP socket
static constexpr int LISTEN_BACKLOG = 1024;

// buffer config
static constexpr size_t BACK_PRESSURE = 128 * 1024;              // high watermark of the send queue
static constexpr size_t LOW_WATERMARK = 64 * 1024;               // low watermark of the send queue
static constexpr size_t READ_BUFFER_SIZE = 64 * 1024;            // shared read buffer of the loop
static constexpr size_t SEND_BLOCK_MIN = 4 * 1024;               // minimum block size in SendQueue
static constexpr size_t MAX_SEND_QUEUE = 64 * 1024 * 1024;       // hard cap of one socket's send queue

// Loop maintenance (connect/idle/linger timeouts, mux keep-alive): the loop wakes up at least this often
constexpr int MAINTENANCE_INTERVAL_MS = 1000;

// Timeouts in seconds
constexpr int CLOSING_TIMEOUT_SECS = 60;     // max time to flush the send queue on close()
constexpr int LINGER_TIMEOUT_SECS = 5;       // max wait for the FIN of the peer
constexpr int CONNECT_TIMEOUT_SECS = 15;     // DNS + connect + TLS handshake
constexpr int ACCEPT_RETRY_SECS = 1;         // accept pause after running out of descriptors

// Keep-Alive (TCP level)
constexpr int KEEPIDLE_SECS = 240;   // idle time before the first probe
constexpr int KEEPINTVL_SECS = 30;   // time between probes
constexpr int KEEPCNT_MAX = 4;       // number of probes

} // namespace netrector

#endif // CONSTANTS_H
