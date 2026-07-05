/**
 * @file    udp.c
 * @brief   UDP socket wrapper implementation.
 *
 * Cross-platform: Windows (Winsock2) and POSIX (BSD sockets).
 */

#include "udp.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  #define CLOSE_SOCKET(s) closesocket(s)
  #define SOCKET_ERROR_VAL SOCKET_ERROR
  static int g_winsock_initialized = 0;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #define CLOSE_SOCKET(s) close(s)
  #define SOCKET_ERROR_VAL (-1)
  typedef int SOCKET;
#endif

struct UDPSocket {
    SOCKET fd;
    int    port;
};

/* =========================================================================
 *  Winsock initialization (Windows only)
 * ========================================================================= */

#ifdef _WIN32
static int winsock_init(void)
{
    if (g_winsock_initialized) return 0;

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        LOG_ERROR("WSAStartup failed");
        return -1;
    }
    g_winsock_initialized = 1;
    LOG_DEBUG("Winsock initialized");
    return 0;
}

__attribute__((unused))
static void winsock_cleanup(void)
{
    if (g_winsock_initialized) {
        WSACleanup();
        g_winsock_initialized = 0;
    }
}
#endif

/* =========================================================================
 *  Socket operations
 * ========================================================================= */

UDPSocket* udp_socket_create(int local_port)
{
#ifdef _WIN32
    if (winsock_init() != 0) return NULL;
#endif

    UDPSocket* sock = calloc(1, sizeof(UDPSocket));
    if (!sock) {
        LOG_ERROR("Out of memory allocating UDPSocket");
        return NULL;
    }

    sock->fd = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (sock->fd == INVALID_SOCKET) {
#else
    if (sock->fd < 0) {
#endif
        LOG_ERROR("socket() failed: %d",
#ifdef _WIN32
                  WSAGetLastError()
#else
                  errno
#endif
        );
        free(sock);
        return NULL;
    }

    /* Allow address reuse */
    int reuse = 1;
    setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    /* Set non-blocking mode */
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock->fd, FIONBIO, &mode);
#else
    fcntl(sock->fd, F_SETFL, fcntl(sock->fd, F_GETFL, 0) | O_NONBLOCK);
#endif

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)local_port);

    if (bind(sock->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind() port %d failed: %d", local_port,
#ifdef _WIN32
                  WSAGetLastError()
#else
                  errno
#endif
        );
        CLOSE_SOCKET(sock->fd);
        free(sock);
        return NULL;
    }

    sock->port = local_port;
    LOG_INFO("UDP socket bound to port %d", local_port);
    return sock;
}

void udp_socket_destroy(UDPSocket* sock)
{
    if (!sock) return;
    if (sock->fd != (SOCKET)(-1)) {
        CLOSE_SOCKET(sock->fd);
    }
    free(sock);
    LOG_DEBUG("UDP socket destroyed");
}

int udp_socket_recv(UDPSocket* sock, uint8_t* buf, int buf_size, int timeout_ms)
{
    if (!sock || !buf || buf_size <= 0) return -1;

    /* Use select() for timeout */
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock->fd, &read_fds);

    struct timeval tv;
    struct timeval* tv_ptr = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tv_ptr = &tv;
    }

    int sel_ret = select((int)(sock->fd + 1), &read_fds, NULL, NULL, tv_ptr);
    if (sel_ret < 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        LOG_WARN("select() error: WSA code %d (fd=%d)", err, (int)sock->fd);
#else
        LOG_WARN("select() error: errno=%d (fd=%d)", errno, sock->fd);
#endif
        return -1;
    }
    if (sel_ret == 0) {
        return 0;  /* Timeout */
    }

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock->fd, (char*)buf, buf_size, 0,
                     (struct sockaddr*)&from, &from_len);
    if (n < 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 0;
        LOG_WARN("recvfrom() error: WSA code %d", err);
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        LOG_WARN("recvfrom() error: errno=%d", errno);
#endif
        return -1;
    }

    return n;
}

int udp_socket_sendto(UDPSocket* sock, const uint8_t* data, int len,
                      const char* dest_ip, int dest_port)
{
    if (!sock || !data || len <= 0 || !dest_ip) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)dest_port);

    if (inet_pton(AF_INET, dest_ip, &addr.sin_addr) != 1) {
        LOG_WARN("Invalid IP address: %s", dest_ip);
        return -1;
    }

    int n = sendto(sock->fd, (const char*)data, len, 0,
                   (struct sockaddr*)&addr, sizeof(addr));
    if (n < 0) {
        LOG_WARN("sendto() to %s:%d failed", dest_ip, dest_port);
        return -1;
    }

    return n;
}
