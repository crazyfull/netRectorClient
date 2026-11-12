// Functional tests on loopback: TCP, half-close/close, UDP, DNS, TLS, mux, back-pressure.
// Everything runs on one EventLoop (client and server side) driven by runOnce().
#include "netRector.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <functional>

using namespace netrector;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool pump(EventLoop& loop, const std::function<bool()>& done, int maxMs = 5000)
{
    uint64_t start = platform::monotonicMs();
    while (!done()) {
        if (platform::monotonicMs() - start > (uint64_t)maxMs) return false;
        loop.runOnce(20);
    }
    return true;
}

// ---- echo server socket / collecting client socket ----
// server side sockets are owned (and deleted) by the loop: only a counter is kept, never a pointer
static int g_serverClosed = 0;
struct EchoSocket : TCPSocket {
    void onReceiveData(const uint8_t* d, size_t n) override { send(d, n); }
    void onClose() override { g_serverClosed++; }
};
static TCPSocket* makeEcho(void*) { return new EchoSocket; }

struct ClientSocket : TCPSocket {
    std::string got; bool connected = false, closed = false, failed = false;
    void onConnected() override { connected = true; }
    void onConnectFailed() override { failed = true; }
    void onReceiveData(const uint8_t* d, size_t n) override { got.append((const char*)d, n); }
    void onClose() override { closed = true; }
};

static void testTcp()
{
    printf("[tcp] echo + close\n");
    EventLoop loop;
    Listener l(&loop);
    CHECK(l.start(0, "127.0.0.1", makeEcho, nullptr));
    ClientSocket* c = new ClientSocket; c->setAutoDelete(false); c->setReactor(&loop);
    CHECK(c->connectTo("127.0.0.1", (uint16_t)l.port()));
    CHECK(pump(loop, [&]{ return c->connected; }));
    c->send("hello", 5);
    CHECK(pump(loop, [&]{ return c->got == "hello"; }));

    // large transfer through the send queue (back-pressure path)
    std::string big(4 * 1024 * 1024, 'x');
    for (size_t i = 0; i < big.size(); i++) big[i] = (char)('a' + i % 26);
    c->got.clear();
    // default onPause pauses reading of the same socket, so use the app callbacks: ignore pause
    c->setOnPause([](void*){}, nullptr);
    CHECK(c->send(big.data(), big.size()));
    CHECK(pump(loop, [&]{ return c->got.size() >= big.size(); }, 15000));
    CHECK(c->got == big);

    c->close(false);
    CHECK(pump(loop, [&]{ return c->closed; }));
    CHECK(c->closeReason() == TCPSocket::ClosedByApp);
    CHECK(pump(loop, [&]{ return g_serverClosed >= 1; }));
    delete c;
    l.stop();
}

static void testConnectFailed()
{
    printf("[tcp] connect refused + DNS failure\n");
    EventLoop loop;
    ClientSocket* c = new ClientSocket; c->setReactor(&loop);
    CHECK(c->connectTo("127.0.0.1", 1));   // nothing listens on port 1
    bool done = false;
    c->setAutoDelete(false);
    CHECK(pump(loop, [&]{ done = c->failed && c->closed; return done; }));
    CHECK(c->closeReason() == TCPSocket::ConnectFailed);
    delete c;

    ClientSocket* d = new ClientSocket; d->setReactor(&loop); d->setAutoDelete(false);
    CHECK(d->connectTo("no-such-host.invalid", 80));
    CHECK(pump(loop, [&]{ return d->failed && d->closed; }, 20000));
    delete d;
}

static void testDnsLocalhost()
{
    printf("[dns] localhost\n");
    EventLoop loop;
    Listener l(&loop);
    CHECK(l.start(0, "127.0.0.1", makeEcho, nullptr));
    ClientSocket* c = new ClientSocket; c->setReactor(&loop); c->setAutoDelete(false);
    CHECK(c->connectTo("localhost", (uint16_t)l.port()));
    CHECK(pump(loop, [&]{ return c->connected || c->failed; }));
    CHECK(c->connected);
    c->close(true);
    delete c; l.stop();
}

struct UdpEcho : UDPSocket {
    void onDatagram(const uint8_t* d, size_t n, const sockaddr* from, socklen_t fl) override { sendTo(d, n, from, fl); }
};
struct UdpClient : UDPSocket {
    std::string got;
    void onDatagram(const uint8_t* d, size_t n, const sockaddr*, socklen_t) override { got.append((const char*)d, n); }
};
static void testUdp()
{
    printf("[udp] echo\n");
    EventLoop loop;
    UdpEcho* s = new UdpEcho; s->setReactor(&loop);
    CHECK(s->bind("127.0.0.1", 0));
    uint16_t port = s->getLocalPort();
    CHECK(port != 0);
    UdpClient* c = new UdpClient; c->setReactor(&loop); c->setAutoDelete(false);
    CHECK(c->connect("127.0.0.1", port));
    CHECK(c->send("ping", 4));
    CHECK(pump(loop, [&]{ return c->got == "ping"; }));
    c->close(); delete c;
    s->close();
}

static void testTls(const std::string& dir)
{
    printf("[tls] handshake + echo\n");
    if (!TLSContext::isSupported()) { printf("  skipped (no TLS)\n"); return; }
    EventLoop loop;
    std::string err;
    TLSContext* srv = TLSContext::createServer((dir + "/cert.pem").c_str(), (dir + "/key.pem").c_str(), &err);
    CHECK(srv != nullptr); if (!srv) { printf("  %s\n", err.c_str()); return; }
    srv->setALPN({"h2", "http/1.1"});
    static TLSContext* s_srv; s_srv = srv;
    Listener l(&loop);
    CHECK(l.start(0, "127.0.0.1", [](void*) -> TCPSocket* {
        EchoSocket* s = new EchoSocket; s->enableTLS(s_srv); return s; }, nullptr));

    // verification on, self-signed: must fail
    TLSContext* strict = TLSContext::createClient(true, (dir + "/other-ca.pem").c_str(), &err);
    CHECK(strict != nullptr);
    if (strict) {
        ClientSocket* bad = new ClientSocket; bad->setReactor(&loop); bad->setAutoDelete(false);
        CHECK(bad->enableTLS(strict, "localhost"));
        CHECK(bad->connectTo("127.0.0.1", (uint16_t)l.port()));
        CHECK(pump(loop, [&]{ return bad->failed && bad->closed; }));
        CHECK(!bad->connected);
        delete bad;
    }

    // trusting the generated CA: must work, with SNI/host check against "localhost"
    TLSContext* good = TLSContext::createClient(true, (dir + "/cert.pem").c_str(), &err);
    CHECK(good != nullptr);
    if (good) {
        good->setALPN({"http/1.1"});
        ClientSocket* c = new ClientSocket; c->setReactor(&loop); c->setAutoDelete(false);
        CHECK(c->enableTLS(good, "localhost"));
        CHECK(c->connectTo("127.0.0.1", (uint16_t)l.port()));
        CHECK(pump(loop, [&]{ return c->connected || c->failed; }));
        CHECK(c->connected);
        CHECK(c->tlsALPN() == "http/1.1");
        std::string big(1500000, 'q');
        c->setOnPause([](void*){}, nullptr);
        CHECK(c->send(big.data(), big.size()));
        CHECK(pump(loop, [&]{ return c->got.size() >= big.size(); }, 15000));
        CHECK(c->got == big);
        c->close(false);
        CHECK(pump(loop, [&]{ return c->closed; }));
        delete c;
        delete good;
    }
    l.stop(); delete strict;
    loop.closeAll(); delete srv;
}

// ---- mux: client opens 3 streams, server echoes each ----
struct MuxServer : MultiplexedTunnel {
    MuxServer() : MultiplexedTunnel(false) {}
};
static void muxServerData(void*, uint32_t, const uint8_t*, size_t) {}
static MuxServer* g_mux = nullptr;
struct StreamEcho { MuxServer* t; };
static void muxEchoData(void* a, uint32_t id, const uint8_t* d, size_t n) {
    static_cast<MuxServer*>(a)->sendToStream(id, d, n);
}
static void muxEchoClose(void*, uint32_t) {}
static void onNewStream(void* a, uint32_t id, MultiplexedTunnel::Stream* s) {
    s->onData = muxEchoData; s->onClose = muxEchoClose; s->arg = a; (void)id;
}
struct MuxClient : MultiplexedTunnel {
    MuxClient() : MultiplexedTunnel(true) {}
    bool up = false;
    void onConnected() override { up = true; }
};
struct StreamCollector { std::string got; bool closed = false; };
static void colData(void* a, uint32_t, const uint8_t* d, size_t n) { static_cast<StreamCollector*>(a)->got.append((const char*)d, n); }
static void colClose(void* a, uint32_t) { static_cast<StreamCollector*>(a)->closed = true; }

static void testMux()
{
    printf("[mux] 3 streams echo + keepalive\n");
    EventLoop loop;
    Listener l(&loop);
    CHECK(l.start(0, "127.0.0.1", [](void*) -> TCPSocket* {
        MuxServer* s = new MuxServer; g_mux = s; s->setOnNewStream(onNewStream, s); return s; }, nullptr));
    MuxClient* c = new MuxClient; c->setReactor(&loop); c->setAutoDelete(false);
    c->setKeepAlive(1);
    CHECK(c->connectTo("127.0.0.1", (uint16_t)l.port()));
    CHECK(pump(loop, [&]{ return c->up; }));
    StreamCollector col[3];
    uint32_t ids[3];
    for (int i = 0; i < 3; i++) { ids[i] = c->openStream(colData, colClose, &col[i]); CHECK(ids[i] != 0); }
    std::string payload(600000, 'm');   // larger than the 256 KB window: exercises flow control
    for (int i = 0; i < 3; i++) CHECK(c->sendToStream(ids[i], (const uint8_t*)payload.data(), payload.size()));
    CHECK(pump(loop, [&]{ return col[0].got.size() == payload.size() && col[1].got.size() == payload.size() && col[2].got.size() == payload.size(); }, 15000));
    for (int i = 0; i < 3; i++) c->closeStream(ids[i]);
    CHECK(pump(loop, [&]{ return col[0].closed && col[1].closed && col[2].closed; }));

    // keep-alive must keep an idle tunnel alive for a few ticks
    uint64_t t0 = platform::monotonicMs();
    while (platform::monotonicMs() - t0 < 3500) loop.runOnce(100);
    CHECK(c->getStatus() == TCPSocket::Connected);
    c->close(true); delete c; l.stop();
}

static void testLoopPost()
{
    printf("[loop] post() from another thread + stop()\n");
    EventLoop loop;
    std::atomic<int> n{0};
    std::thread t([&]{ for (int i = 0; i < 100; i++) loop.post([&]{ n++; }); loop.post([&]{ loop.stop(); }); });
    loop.run();
    t.join();
    CHECK(n.load() == 100);
}

int main(int argc, char** argv)
{
    setLogLevel(LOG_WARN);
    std::string dir = argc > 1 ? argv[1] : ".";
    testLoopPost();
    testTcp();
    testConnectFailed();
    testDnsLocalhost();
    testUdp();
    testTls(dir);
    testMux();
    printf(g_fail ? "FAILED (%d)\n" : "ALL OK\n", g_fail);
    return g_fail ? 1 : 0;
}
