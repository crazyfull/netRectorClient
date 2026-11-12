#ifndef CLSTCPSOCKET_H
#define CLSTCPSOCKET_H
//
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "SocketContext.h"
#include "clsDNSLookup.h"
#include "clsPlatform.h"
#include "clsReactorObject.h"

// forward declaration (BoringSSL)
typedef struct ssl_st SSL;

namespace netrector {

class EventLoop;
class Listener;
class TLSContext;

// ============================== TCPSocket ====================================
// Same public API as netRector's TCPSocket (setReactor takes an EventLoop; EpollReactor is an
// alias of it in clsEpollReactor.h).
//
// Ownership and lifetime:
//   - create the object with new. After accept, or after connectTo() == true, the loop owns it
//     and deletes it after close (deleteLater).
//   - onClose is called exactly once, as the last callback. Do not touch the object afterwards.
//   - if the socket is a member of another class call setAutoDelete(false); the owner then
//     deletes it after onClose, never inside it.
//   - every method must be called from the thread of the loop (from another thread: EventLoop::post).
class TCPSocket : public ReactorObject, public IoObject
{
public:
    TCPSocket();
    enum socketStatus{
        Ready = 1,
        Closed = 2,
        Closing = 3,        // flushing the send queue / waiting for the FIN of the peer
        Connecting = 4,     // DNS or TCP connect
        Connected = 5,
        Handshaking = 6     // TLS handshake (accept and connect)
    };

    // why the socket was closed (readable in onClose with closeReason() and getErrorCode())
    enum CloseReason {
        NotClosed = 0,
        ClosedByApp = 1,        // close() by the application
        ClosedByPeer = 2,       // FIN / RST of the peer
        ClosedByError = 3,      // I/O or TLS error (getErrorCode)
        ClosedByTimeout = 4,    // connect/handshake/idle timeout
        ConnectFailed = 5       // connect or DNS failed (onConnectFailed was called too)
    };

    void setReactor(EventLoop* r);

    using OnDataFn = void(*)(void* , const uint8_t* data, size_t len);
    using OnConnectingFn = void(*)(void* p);

    using OnAcceptedFn = void(*)(void*);
    using OnCloseFn = void(*)(void*);
    using OnConnectedFn = void(*)(void*);
    using OnConnectFailedFn = void(*)(void*);

    using OnPauseFn = void(*)(void* p);
    using OnResumeFn = void(*)(void* p);
    using OnEOFFn = void(*)(void* p);


    void setOnData(OnDataFn fn, void *Arg);
    void setOnConnecting(OnConnectingFn fn, void* Arg);
    void setOnConnected(OnConnectedFn fn, void *Arg);

    void setOnAccepted(OnAcceptedFn fn, void* Arg);
    void setOnClose(OnCloseFn fn, void* Arg);
    void setOnConnectFailed(OnConnectFailedFn fn, void* Arg);

    // onPause: the send queue of this socket grew past BACK_PRESSURE (stop feeding it)
    // onResume: the send queue drained to LOW_WATERMARK
    void setOnPause(OnPauseFn fn, void* Arg);
    void setOnResume(OnResumeFn fn, void* Arg);

    // only with setAllowHalfClose(true): the peer sent FIN
    void setOnEOF(OnEOFFn fn, void* Arg);


    virtual ~TCPSocket();
    virtual void onAccepted() {}                                // with TLS: after the handshake
    virtual void onClose() {}                                   // always the last callback
    virtual void onConnectFailed(){}                            // before onClose
    virtual void onConnecting(){}
    virtual void onConnected(){}                                // with TLS: after the handshake
    virtual void onReceiveData(const uint8_t* /*data*/, size_t /*len*/){}
    virtual void onPause();                                     // default: pause_reading()
    virtual void onResume();                                    // default: resume_reading()
    virtual void onReadEOF(){}


    CloseReason closeReason() const { return m_closeReason; }

    // hard cap of this socket's send queue (0 = MAX_SEND_QUEUE). send() is refused above it.
    void setMaxSendQueue(size_t bytes) { m_maxSendQueue = bytes; }

    // false: not connected or error; nothing was sent. An error after this call is reported with onClose.
    bool send(const void * data, size_t len);
    bool sendv(const struct iovec* iov, int iovcnt);

    // zero-copy: the application memory is sent directly; release is called when it is not needed
    // any more (also on close). release == nullptr for static data.
    bool sendNoCopy(const void* data, size_t len, SendQueue::ReleaseFn release = nullptr, void* arg = nullptr);

    // force=false: flush the send queue, send FIN, close (onClose at the end)
    // force=true : close now and drop the send queue
    void close(bool force = false);

    // half-close: FIN is sent when the send queue is empty, reading continues
    void shutdownWrite();

    // true: the result (success or not) comes with callbacks, possibly before this returns.
    // false: no callback was called and the object still belongs to the caller.
    bool connectTo(const char *host, uint16_t port);

    // ---- TLS (BoringSSL) ----
    // call before connectTo() or inside the accept callback.
    // serverName (optional, client): SNI and certificate name; default: the host of connectTo
    bool enableTLS(TLSContext* ctx, const char* serverName = nullptr);
    bool isTLS() const;
    TLSContext* tlsContext() const { return m_tlsCtx; }
    const char* tlsVersion() const;
    std::string tlsALPN() const;


    // accessors
    int fd() const;
    NrSocket handle() const { return m_SocketContext.fd; }
    TCPSocket *getPointer();
    uint64_t getLastActiveTime();
    uint16_t getLocalPort();
    bool getPeerAddress(std::string& ip, uint16_t& port) const;
    size_t pendingBytes() const;
    bool isWriteBlocked() const;
    bool isAccepted() const;

    static void setSocketOption(NrSocket fd, int name, bool isEnable);
    static void setSocketShared(NrSocket fd, bool isEnable);        // SO_REUSEPORT (where it exists)
    static int setSocketNonblocking(NrSocket fd);
    static void setSocketResourceAddress(NrSocket fd, bool isEnable);
    static void setSocketNoDelay(NrSocket fd, bool isEnable);
    static void setSocketCork(NrSocket fd, bool isEnable);          // TCP_CORK (Linux/Android only)
    static void setSocketKeepAlive(NrSocket fd, bool isEnable);
    static void setSocketLowDelay(NrSocket fd, bool isEnable);


    SocketContext *getSocketContext() ;

    void pause_reading();
    void resume_reading();
    bool isReadPaused() const;
    int getErrorCode();

    // false (default): FIN of the peer closes the socket (after the send queue drained)
    // true: onReadEOF is called and the socket stays open for writing
    void setAllowHalfClose(bool enable);

    // false: the loop does not delete the object after close
    void setAutoDelete(bool enable);

    // 0 = off. Connected sockets without any activity for this long are closed
    void setIdleTimeout(uint32_t seconds);

    socketStatus getStatus() const;
    EventLoop *getReactor() const;

    // ---- internal ----
    friend class EventLoop;
    friend class Listener;

protected:
    OnDataFn m_onData { nullptr };
    OnAcceptedFn m_onAccepted { nullptr };
    OnCloseFn m_onClose { nullptr };
    OnConnectFailedFn m_onConnectFailed { nullptr };
    OnConnectingFn m_onConnecting { nullptr };
    OnConnectedFn m_onConnected { nullptr };

    OnPauseFn m_onPause { nullptr };
    OnResumeFn m_onResume { nullptr };
    OnEOFFn m_onEOF { nullptr };

    // one argument for all callbacks (the last setOn* wins): the callbacks of a socket always
    // belong to one owner object.
    void* m_callbacksArg { nullptr };

    struct SocketContext m_SocketContext {};
    void setStatus(socketStatus newStatus);

    // dispatchers: the function-pointer callback if set, otherwise the virtual method.
    // Protocol layers (MultiplexedTunnel) override these to stay independent of the app callbacks.
    virtual void handleOnData(const uint8_t * d, size_t n);
    virtual void handleOnAccepted();
    virtual void handleOnClose();
    virtual void handleOnConnectFailed();
    virtual void handleOnConnecting();
    virtual void handleOnConnected();
    virtual void handleOnPause();
    virtual void handleOnResume();
    virtual void handleOnEOF();

    // about once per second (loop maintenance), after the timeout checks. MultiplexedTunnel uses it
    // for its keep-alive.
    virtual void onTick(uint64_t /*nowSeconds*/) {}

    // close at the end of the current loop iteration (without running onClose in the middle of the caller)
    void abortLater();

private:
    // IoObject
    void onIoEvents(uint32_t ev) override;
    void onDeferred(int kind) override;
    void onMaintenance(uint64_t nowSeconds) override;
    void onLoopShutdown() override;

    void checkTimeout(uint64_t now);
    void onReadable();
    void onWritable();
    void _accepted(NrSocket fd);

    EventLoop* m_pReactor = nullptr;
    socketStatus status {Ready};
    CloseReason m_closeReason { NotClosed };
    size_t m_maxSendQueue { 0 };

    bool m_readPaused { false };
    bool m_pendingClose { false };      // close(false): close when the send queue is empty
    bool m_lingering { false };         // FIN sent, waiting for the FIN of the peer
    bool m_readEOF { false };           // FIN received
    bool m_shutdownWrPending { false }; // shutdownWrite() called
    bool m_writeShutdown { false };     // FIN sent
    bool m_writeBlocked { false };      // send queue is over BACK_PRESSURE (onPause was called)
    bool m_abortPending { false };      // error inside send(); the close happens later in the loop
    bool m_deferredRead { false };
    bool m_registered { false };        // registered in the loop
    bool m_isAccepted { false };
    bool m_autoDelete { true };
    bool m_allowHalfClose { false };
    bool m_dnsPending { false };
    bool m_connectFailNotified { false };
    bool m_tlsReadWantsWrite { false };
    bool m_tlsWriteWantsRead { false };
    bool m_tlsHandshakeWantsWrite { false };

    int m_lastError { 0 };
    uint32_t m_idleTimeout { 0 };
    uint32_t m_interest { 0 };          // interest currently set in the loop

    TLSContext* m_tlsCtx { nullptr };
    SSL* m_ssl { nullptr };

    // only for outgoing connections / TLS with a name (accepted connections do not need it)
    struct ConnectInfo {
        std::string host;
        std::string serverName;
        std::vector<std::string> addrs;     // DNS answers (up to 8)
        size_t nextAddr { 0 };
    };
    ConnectInfo* m_connInfo { nullptr };
    ConnectInfo& connInfo();

    void updateLastActive();
    uint32_t wantedInterest() const;
    void updateInterest();
    bool registerSocket(NrSocket fd, uint32_t interest);
    void unregisterSocket();

    // connect
    static void connect_cb(const char *hostname, char **ips, size_t count, DNSLookup::QUERY_TYPE qtype, void *p);
    void onResolved(char **ips, size_t count);
    void startConnect(const struct sockaddr* addr, socklen_t addrLen);
    void onConnectReady();
    void failConnect(int err);
    bool tryNextAddress(int err);

    // close
    void finalizeClose();
    void drainComplete();
    void doShutdownWrite();
    void lingerDiscard();
    void handleReadEOF();

    // io (plain or TLS)
    ssize_t ioRead(char* buf, size_t cap);
    ssize_t ioWrite(const void* data, size_t len);
    bool flushQueue();
    bool queueHasRoom(size_t len);
    int socketError();

    // TLS
    bool tlsStart(bool isServer);
    void tlsHandshake();
};

} // namespace netrector

#endif // CLSTCPSOCKET_H
