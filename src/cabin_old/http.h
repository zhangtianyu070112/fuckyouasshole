/**
 * @file    http.h
 * @brief   Minimal HTTP client for REST API calls.
 *
 * Supports HTTP GET only (sufficient for Amap API).
 * Uses blocking sockets — call from a background thread to avoid
 * stalling the render loop.
 *
 * Example:
 *   HTTPResponse* resp = http_get("restapi.amap.com", 80,
 *                                  "/v3/weather/weatherInfo?key=xxx&city=110000");
 *   // use resp->body
 *   http_response_free(resp);
 */

#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

/* --- Response ---------------------------------------------------------- */

typedef struct {
    int    status_code;    /* e.g. 200, 404, 0 = connection error */
    char*  body;           /* Response body (null-terminated) */
    size_t body_len;       /* Length of body */
    char   error_msg[256]; /* Error description if status_code == 0 */
} HTTPResponse;

/**
 * @brief Perform an HTTP GET request.
 * @param host  Hostname or IP (e.g. "restapi.amap.com").
 * @param port  Port (typically 80 for HTTP).
 * @param path  Request path with query string (e.g. "/v3/...").
 * @return Heap-allocated response. Caller must http_response_free().
 */
HTTPResponse* http_get(const char* host, int port, const char* path);

/**
 * @brief Free an HTTP response.
 */
void http_response_free(HTTPResponse* resp);

#endif /* HTTP_H */
