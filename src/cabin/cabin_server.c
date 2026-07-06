/**
 * @file    cabin_server.c
 * @brief   Lightweight HTTP server for the cabin moving-map display.
 *
 * Serves static files from a configurable www-root and provides JSON
 * API endpoints consumed by the 高德 JS API 2.0 frontend:
 *   GET /api/position  — current aircraft position / motion
 *   GET /api/route     — active flight-plan route
 *   GET /api/config    — frontend configuration (API key, intervals)
 *   GET /*              — static file from www_root
 */

#include "cabin_server.h"
#include "utils/logger.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define HTTP_HEADER_MAX    256
#define RESPONSE_BODY_MAX  (128 * 1024)   /* 128 KiB for JSON + files */
#define RESPONSE_MAX       (RESPONSE_BODY_MAX + HTTP_HEADER_MAX)
#define REQUEST_MAX        4096
#define SMALL_BUF          512

/* ------------------------------------------------------------------ */
/*  CabinServer struct                                                */
/* ------------------------------------------------------------------ */

struct CabinServer {
    int            port;
    char           www_root[256];
    char           amap_js_key[128];
    FMCState*      fmc;
    SDL_Thread*    thread;
    SDL_atomic_t   running;

#ifdef _WIN32
    SOCKET         server_socket;
#else
    int            server_socket;
#endif

    SDL_mutex*     data_mutex;
    FlightDataValues last_fd;
};

/* ------------------------------------------------------------------ */
/*  Helpers – MIME type                                               */
/* ------------------------------------------------------------------ */

static const char* mime_type(const char* path)
{
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if      (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    else if (strcmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    else if (strcmp(ext, ".js")   == 0) return "application/javascript; charset=utf-8";
    else if (strcmp(ext, ".json") == 0) return "application/json; charset=utf-8";
    else if (strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    else if (strcmp(ext, ".png")  == 0) return "image/png";
    else if (strcmp(ext, ".jpg")  == 0) return "image/jpeg";
    else if (strcmp(ext, ".ico")  == 0) return "image/x-icon";
    else if (strcmp(ext, ".woff2")== 0) return "font/woff2";
    else if (strcmp(ext, ".woff") == 0) return "font/woff";
    else                                return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/*  URL-decode helper (in-place)                                      */
/* ------------------------------------------------------------------ */

static void url_decode(char* dst, const char* src)
{
    char* out = dst;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            *out++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *out++ = ' ';
            src++;
        } else {
            *out++ = *src++;
        }
    }
    *out = '\0';
}

/* ------------------------------------------------------------------ */
/*  API handlers                                                      */
/* ------------------------------------------------------------------ */

static void api_position(CabinServer* server, char* body, size_t max_len)
{
    SDL_LockMutex(server->data_mutex);
    FlightDataValues fd = server->last_fd;
    SDL_UnlockMutex(server->data_mutex);

    snprintf(body, max_len,
        "{"
        "\"lat\": %.6f, \"lon\": %.6f, \"alt_ft\": %.1f, "
        "\"hdg\": %.1f, \"gs_kts\": %.1f, \"tas_kts\": %.1f, "
        "\"oat_c\": %.1f, \"vs_fpm\": %.1f, \"mach\": %.3f"
        "}",
        fd.lat_deg, fd.lon_deg, fd.alt_msl_ft,
        fd.heading_true_deg, fd.gs_kts, fd.tas_kts,
        fd.oat_c, fd.vs_fpm, fd.mach);
}

static void api_route(CabinServer* server, char* body, size_t max_len)
{
    if (!server->fmc) {
        snprintf(body, max_len,
            "{\"dep_icao\":\"\",\"arr_icao\":\"\","
            "\"active_idx\":-1,\"total_distance_nm\":0.0,\"waypoints\":[]}");
        return;
    }

    fmc_state_lock(server->fmc);
    FlightPlan* fp = &server->fmc->flight_plan;

    /* Open JSON */
    int pos = snprintf(body, max_len,
        "{\"dep_icao\":\"%s\",\"arr_icao\":\"%s\","
        "\"active_idx\":%d,\"total_distance_nm\":%.1f,"
        "\"waypoints\":[",
        fp->departure.icao, fp->arrival.icao,
        fp->active_waypoint_index, fp->total_distance_nm);

    /* Waypoints array */
    for (int i = 0; i < fp->waypoint_count && (size_t)pos < max_len - 256; i++) {
        char buf[320];
        int n = snprintf(buf, sizeof(buf),
            "{\"ident\":\"%s\",\"lat\":%.6f,\"lon\":%.6f}%s",
            fp->waypoints[i].ident,
            fp->waypoints[i].pos.lat_deg,
            fp->waypoints[i].pos.lon_deg,
            (i == fp->waypoint_count - 1) ? "" : ",");
        if (pos + n < (int)max_len - 1) {
            memcpy(body + pos, buf, (size_t)n);
            pos += n;
        }
    }
    fmc_state_unlock(server->fmc);

    /* Close */
    if ((size_t)pos + 3 < max_len) {
        memcpy(body + pos, "]}", 3);
    }
}

static void api_config(CabinServer* server, char* body, size_t max_len)
{
    snprintf(body, max_len,
        "{"
        "\"amap_js_key\":\"%s\","
        "\"update_interval_ms\":1000"
        "}",
        server->amap_js_key);
}

/* ------------------------------------------------------------------ */
/*  Static file serving                                               */
/* ------------------------------------------------------------------ */

static int serve_static_file(CabinServer* server, const char* url_path,
                             char* response, size_t resp_max)
{
    /* Build filesystem path: www_root + url_path.
     * Default url_path "/" → "/index.html" */
    if (strcmp(url_path, "/") == 0)
        url_path = "/index.html";

    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s%s", server->www_root, url_path);

    /* Security: block attempts to escape www_root */
    if (strstr(url_path, "..") != NULL) {
        snprintf(response, resp_max,
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n\r\n"
            "403 Forbidden");
        return (int)strlen(response);
    }

    FILE* f = fopen(file_path, "rb");
    if (!f) {
        snprintf(response, resp_max,
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n\r\n"
            "404 Not Found");
        return (int)strlen(response);
    }

    /* Read file into body area (after header placeholder) */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > (long)(resp_max - HTTP_HEADER_MAX - 1)) {
        fclose(f);
        snprintf(response, resp_max,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n\r\n"
            "500 File too large or empty");
        return (int)strlen(response);
    }

    const char* mime = mime_type(file_path);
    int header_len = snprintf(response, HTTP_HEADER_MAX,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        mime, fsize);

    size_t nread = fread(response + header_len, 1, (size_t)fsize, f);
    fclose(f);

    return header_len + (int)nread;
}

/* ------------------------------------------------------------------ */
/*  Request routing                                                   */
/* ------------------------------------------------------------------ */

static int handle_request(CabinServer* server, const char* method,
                          const char* url_raw, char* response, size_t resp_max)
{
    (void)method;  /* Only GET is used; ignore others */

    char url_path[512];
    url_decode(url_path, url_raw);

    /* Strip query string */
    char* q = strchr(url_path, '?');
    if (q) *q = '\0';

    /* --- API routes --- */
    if (strcmp(url_path, "/api/position") == 0) {
        char body[SMALL_BUF];
        api_position(server, body, sizeof(body));
        return snprintf(response, resp_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n%s", body);
    }

    if (strcmp(url_path, "/api/route") == 0) {
        char* body = (char*)malloc(RESPONSE_BODY_MAX);
        int len = 0;
        if (body) {
            api_route(server, body, RESPONSE_BODY_MAX);
            len = snprintf(response, resp_max,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Connection: close\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n%s", body);
            free(body);
        }
        return len;
    }

    if (strcmp(url_path, "/api/config") == 0) {
        char body[SMALL_BUF];
        api_config(server, body, sizeof(body));
        return snprintf(response, resp_max,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n%s", body);
    }

    /* --- Static file fallback --- */
    return serve_static_file(server, url_path, response, resp_max);
}

/* ------------------------------------------------------------------ */
/*  Thread function                                                   */
/* ------------------------------------------------------------------ */

static int cabin_server_thread_func(void* data)
{
    CabinServer* server = (CabinServer*)data;

    while (SDL_AtomicGet(&server->running)) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

#ifdef _WIN32
        SOCKET client_sock = accept(server->server_socket,
                                    (struct sockaddr*)&client_addr, &client_len);
        if (client_sock == INVALID_SOCKET) {
            SDL_Delay(100);
            continue;
        }
#else
        int client_sock = accept(server->server_socket,
                                 (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            SDL_Delay(100);
            continue;
        }
#endif

        char req_buf[REQUEST_MAX];
        int bytes_read = recv(client_sock, req_buf, sizeof(req_buf) - 1, 0);
        if (bytes_read > 0) {
            req_buf[bytes_read] = '\0';

            /* Parse first line: METHOD URL HTTP/... */
            char method[16] = {0};
            char url[512]   = {0};
            sscanf(req_buf, "%15s %511s", method, url);

            char* response = (char*)malloc(RESPONSE_MAX);
            if (response) {
                int resp_len = handle_request(server, method, url,
                                              response, RESPONSE_MAX);
                if (resp_len > 0)
                    send(client_sock, response, resp_len, 0);
                free(response);
            }
        }

#ifdef _WIN32
        closesocket(client_sock);
#else
        close(client_sock);
#endif
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

CabinServer* cabin_server_create(const Config* cfg, FMCState* fmc)
{
    if (!cfg) return NULL;

    /* Check enabled flag */
    if (!config_get_bool(cfg, "cabin", "enabled", 1)) {
        LOG_INFO("CabinServer: Disabled by config");
        return NULL;
    }

    CabinServer* server = (CabinServer*)malloc(sizeof(CabinServer));
    if (!server) return NULL;
    memset(server, 0, sizeof(CabinServer));

    /* Read config */
    server->port = (int)config_get_int(cfg, "cabin", "port", 8080);
    const char* root = config_get_str(cfg, "cabin", "www_root", "resources/cabin");
    strncpy(server->www_root, root, sizeof(server->www_root) - 1);
    const char* key = config_get_str(cfg, "cabin", "amap_js_key", "");
    strncpy(server->amap_js_key, key, sizeof(server->amap_js_key) - 1);

    server->fmc = fmc;
    server->data_mutex = SDL_CreateMutex();
    SDL_AtomicSet(&server->running, 1);

    /* Winsock init (Windows) */
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("CabinServer: WSAStartup failed");
        free(server);
        return NULL;
    }
    server->server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_socket == INVALID_SOCKET) {
        LOG_ERROR("CabinServer: socket() failed");
        WSACleanup();
        free(server);
        return NULL;
    }
#else
    server->server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_socket < 0) {
        LOG_ERROR("CabinServer: socket() failed");
        free(server);
        return NULL;
    }
#endif

    /* Bind */
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)server->port);

    int opt = 1;
#ifdef _WIN32
    setsockopt(server->server_socket, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));
#else
    setsockopt(server->server_socket, SOL_SOCKET, SO_REUSEADDR,
               &opt, sizeof(opt));
#endif

#ifdef _WIN32
    if (bind(server->server_socket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("CabinServer: Bind failed on port %d (err=%d)",
              server->port, WSAGetLastError());
        closesocket(server->server_socket);
        WSACleanup();
        free(server);
        return NULL;
    }
#else
    if (bind(server->server_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("CabinServer: Bind failed on port %d", server->port);
        close(server->server_socket);
        free(server);
        return NULL;
    }
#endif

    listen(server->server_socket, 5);

    /* Non-blocking mode */
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(server->server_socket, FIONBIO, &mode);
#else
    int flags = fcntl(server->server_socket, F_GETFL, 0);
    fcntl(server->server_socket, F_SETFL, flags | O_NONBLOCK);
#endif

    server->thread = SDL_CreateThread(cabin_server_thread_func,
                                      "CabinServerThread", server);
    if (!server->thread) {
        LOG_ERROR("CabinServer: Failed to create thread");
#ifdef _WIN32
        closesocket(server->server_socket);
        WSACleanup();
#else
        close(server->server_socket);
#endif
        free(server);
        return NULL;
    }

    LOG_INFO("CabinServer: Started on port %d (www_root=%s)",
          server->port, server->www_root);
    return server;
}

void cabin_server_destroy(CabinServer* server)
{
    if (!server) return;

    SDL_AtomicSet(&server->running, 0);

    if (server->thread) {
        SDL_WaitThread(server->thread, NULL);
    }

#ifdef _WIN32
    closesocket(server->server_socket);
    WSACleanup();
#else
    close(server->server_socket);
#endif

    if (server->data_mutex) {
        SDL_DestroyMutex(server->data_mutex);
    }

    free(server);
    LOG_INFO("CabinServer: Stopped.");
}

void cabin_server_update_position(CabinServer* server, const FlightData* fd)
{
    if (!server || !fd) return;

    FlightDataValues snapshot;
    flight_data_snapshot((FlightData*)fd, &snapshot);

    SDL_LockMutex(server->data_mutex);
    server->last_fd = snapshot;
    SDL_UnlockMutex(server->data_mutex);
}
