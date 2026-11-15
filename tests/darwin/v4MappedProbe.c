// Darwin experiment: does an AF_INET6 socket (IPV6_V6ONLY=0) connect to a loopback listener
// through an IPv4-mapped address? hev-socks5-tunnel connects to its SOCKS5 server this way.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int listenOn(int family, const char* text, int v6only)
{
    int fd = socket(family, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (family == AF_INET6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        struct sockaddr_in6 a; memset(&a, 0, sizeof(a));
        a.sin6_len = sizeof(a); a.sin6_family = AF_INET6; inet_pton(AF_INET6, text, &a.sin6_addr);
        if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { perror("bind6"); return -1; }
    } else {
        struct sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_len = sizeof(a); a.sin_family = AF_INET; inet_pton(AF_INET, text, &a.sin_addr);
        if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { perror("bind4"); return -1; }
    }
    listen(fd, 8);
    return fd;
}

static int portOf(int fd)
{
    struct sockaddr_storage s; socklen_t l = sizeof(s);
    getsockname(fd, (struct sockaddr*)&s, &l);
    return ntohs(s.ss_family == AF_INET ? ((struct sockaddr_in*)&s)->sin_port : ((struct sockaddr_in6*)&s)->sin6_port);
}

static void tryConnect6(const char* label, const char* text, int port)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    int zero = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    struct sockaddr_in6 a; memset(&a, 0, sizeof(a));
    a.sin6_len = sizeof(a); a.sin6_family = AF_INET6; a.sin6_port = htons(port);
    inet_pton(AF_INET6, text, &a.sin6_addr);
    int r = connect(fd, (struct sockaddr*)&a, sizeof(a));
    printf("%-58s -> %s\n", label, r == 0 ? "OK" : strerror(errno));
    close(fd);
}

int main(void)
{
    int l4 = listenOn(AF_INET, "127.0.0.1", 0);
    tryConnect6("listener AF_INET 127.0.0.1, client v6 -> ::ffff:127.0.0.1", "::ffff:127.0.0.1", portOf(l4));

    int l6 = listenOn(AF_INET6, "::1", 1);
    tryConnect6("listener AF_INET6 ::1, client v6 -> ::1", "::1", portOf(l6));

    int ld = listenOn(AF_INET6, "::", 0);
    tryConnect6("listener dual-stack ::, client v6 -> ::ffff:127.0.0.1", "::ffff:127.0.0.1", portOf(ld));
    tryConnect6("listener dual-stack ::, client v6 -> ::1", "::1", portOf(ld));
    return 0;
}
