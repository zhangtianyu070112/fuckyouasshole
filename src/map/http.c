/**
 * @file    http.c
 * @brief   Minimal HTTP GET client implementation.
 *
 * Uses platform sockets (Winsock2 on Windows, BSD on POSIX).
 * This is intentionally minimal — no TLS, no keep-alive, no chunked encoding.
 * For production HTTPS calls, consider linking against libcurl or WinHTTP.
 */

#include "http.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  #define CLOSE_S(s) closesocket(s)
  #define SOCKERR SOCKET_ERROR
  static int g_ws_started = 0;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netdb.h>
  #include <errno.h>
  #define CLOSE_S(s) close(s)
  #define SOCKERR (-1)
  typedef int SOCKET;
#endif

/* Initial receive buffer. Grows as needed. */
#define HTTP_BUF_INIT  4096
#define HTTP_BUF_MAX   (1024 * 1024)  /* 1 MB max response */

/* =========================================================================
 *  Internal: resolve hostname → IP & connect
 * ========================================================================= */

static int http_connect(const char* host, int port)
{
#ifdef _WIN32
    if (!g_ws_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        g_ws_started = 1;
    }
#endif

    /* Resolve hostname */
    struct hostent* he = gethostbyname(host);
    if (!he) {
        LOG_WARN("HTTP: gethostbyname failed for '%s'", host);
        return -1;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return -1;
#else
    if (s < 0) return -1;
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    /* Set timeout (5 seconds) */
#ifdef _WIN32
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv = { 5, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKERR) {
        CLOSE_S(s);
        return -1;
    }

    return (int)s;
}

/* =========================================================================
 *  Internal: build & send HTTP request
 * ========================================================================= */

static int http_send_request(int sock_fd, const char* host,
                              const char* path)
{
    char req[2048];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: CockpitSim/1.0\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    if (req_len < 0 || req_len >= (int)sizeof(req)) return -1;

#ifdef _WIN32
    return send((SOCKET)sock_fd, req, req_len, 0);
#else
    return (int)send((SOCKET)sock_fd, req, (size_t)req_len, 0);
#endif
}

/* =========================================================================
 *  Internal: receive & parse HTTP response
 * ========================================================================= */

static HTTPResponse* http_recv_response(int sock_fd)
{
    HTTPResponse* resp = calloc(1, sizeof(HTTPResponse));
    if (!resp) return NULL;

    size_t buf_cap = HTTP_BUF_INIT;
    char*  buf     = malloc(buf_cap);
    if (!buf) { free(resp); return NULL; }

    size_t total_read = 0;
    int n;
    while ((n = recv((SOCKET)sock_fd, buf + total_read,
                     (int)(buf_cap - total_read - 1), 0)) > 0) {
        total_read += (size_t)n;
        if (total_read + 512 >= buf_cap) {
            if (buf_cap >= HTTP_BUF_MAX) break;
            buf_cap *= 2;
            if (buf_cap > HTTP_BUF_MAX) buf_cap = HTTP_BUF_MAX;
            char* new_buf = realloc(buf, buf_cap);
            if (!new_buf) break;
            buf = new_buf;
        }
    }
    buf[total_read] = '\0';

    /* Parse status line: "HTTP/1.x NNN ..." */
    const char* p = buf;
    if (strncmp(p, "HTTP/", 5) == 0) {
        p = strchr(p, ' ');
        if (p) {
            resp->status_code = atoi(p + 1);
        }
    }

    /* Find body (after \r\n\r\n) — use byte offset, NOT strlen (binary data!) */
    const char* body_start = strstr(buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t header_len = (size_t)(body_start - buf);
        resp->body_len = (header_len < total_read) ? (total_read - header_len) : 0;
        if (resp->body_len > 0) {
            resp->body = malloc(resp->body_len);
            if (resp->body) {
                memcpy(resp->body, body_start, resp->body_len);
            }
        }
    }

    free(buf);
    return resp;
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

HTTPResponse* http_get(const char* host, int port, const char* path)
{
    if (!host || !path) return NULL;

    int fd = http_connect(host, port);
    if (fd < 0) {
        HTTPResponse* resp = calloc(1, sizeof(HTTPResponse));
        if (resp) {
            resp->status_code = 0;
            snprintf(resp->error_msg, sizeof(resp->error_msg),
                     "Connection failed: %s:%d", host, port);
        }
        return resp;
    }

    if (http_send_request(fd, host, path) < 0) {
        CLOSE_S(fd);
        HTTPResponse* resp = calloc(1, sizeof(HTTPResponse));
        if (resp) {
            resp->status_code = 0;
            snprintf(resp->error_msg, sizeof(resp->error_msg),
                     "Send failed");
        }
        return resp;
    }

    HTTPResponse* resp = http_recv_response(fd);
    CLOSE_S(fd);
    return resp;
}

void http_response_free(HTTPResponse* resp)
{
    if (resp) {
        free(resp->body);
        free(resp);
    }
}
