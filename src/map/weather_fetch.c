/**
 * @file    weather_fetch.c
 * @brief   Fetch weather from Open-Meteo free API via libcurl + cJSON.
 *
 * Open-Meteo is a free, no-API-key global weather service.
 * Replaces the 高德 weather API which only supports Chinese cities.
 */

#include "weather_fetch.h"
#include "utils/logger.h"

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 *  WMO Weather Code → Chinese Description
 *
 *  Reference: https://www.nodc.noaa.gov/archive/arc0021/0002199/1.1/
 *             data/0-data/HTML/WMO-CODE/WMO4677.HTM
 * ========================================================================= */

static const char* wmo_to_chinese(int code)
{
    switch (code) {
        case 0:  return "\xe6\x99\xb4";            /* 晴 */
        case 1:  return "\xe5\xb0\x91\xe4\xba\x91"; /* 少云 */
        case 2:  return "\xe5\xa4\x9a\xe4\xba\x91"; /* 多云 */
        case 3:  return "\xe9\x98\xb4";            /* 阴 */
        case 45:
        case 48: return "\xe9\x9b\xbe";            /* 雾 */
        case 51: return "\xe5\xb0\x8f\xe9\x9b\xa8"; /* 小雨 */
        case 53: return "\xe4\xb8\xad\xe9\x9b\xa8"; /* 中雨 */
        case 55: return "\xe5\xa4\xa7\xe9\x9b\xa8"; /* 大雨 */
        case 56:
        case 57: return "\xe5\x86\xbb\xe9\x9b\xa8"; /* 冻雨 */
        case 61: return "\xe5\xb0\x8f\xe9\x9b\xa8"; /* 小雨 */
        case 63: return "\xe4\xb8\xad\xe9\x9b\xa8"; /* 中雨 */
        case 65: return "\xe5\xa4\xa7\xe9\x9b\xa8"; /* 大雨 */
        case 66:
        case 67: return "\xe5\x86\xbb\xe9\x9b\xa8"; /* 冻雨 */
        case 71: return "\xe5\xb0\x8f\xe9\x9b\xaa"; /* 小雪 */
        case 73: return "\xe4\xb8\xad\xe9\x9b\xaa"; /* 中雪 */
        case 75: return "\xe5\xa4\xa7\xe9\x9b\xaa"; /* 大雪 */
        case 77: return "\xe9\x9b\xaa";            /* 雪 */
        case 80: return "\xe9\x98\xb5\xe9\x9b\xa8"; /* 阵雨 */
        case 81: return "\xe9\x98\xb5\xe9\x9b\xa8"; /* 阵雨 */
        case 82: return "\xe5\xbc\xba\xe9\x98\xb5\xe9\x9b\xa8"; /* 强阵雨 */
        case 85: return "\xe9\x98\xb5\xe9\x9b\xaa"; /* 阵雪 */
        case 86: return "\xe5\xbc\xba\xe9\x98\xb5\xe9\x9b\xaa"; /* 强阵雪 */
        case 95: return "\xe9\x9b\xb7\xe6\x9a\xb4"; /* 雷暴 */
        case 96:
        case 99: return "\xe9\x9b\xb7\xe6\x9a\xb4\xe5\x86\xb0\xe9\x9b\xb9"; /* 雷暴冰雹 */
        default: return "\xe6\x9c\xaa\xe7\x9f\xa5"; /* 未知 */
    }
}

/* =========================================================================
 *  libcurl write callback — append data to a dynamic buffer
 * ========================================================================= */

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} CurlBuf;

static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t total = size * nmemb;
    CurlBuf* buf = (CurlBuf*)userp;
    if (buf->len + total + 1 > buf->cap) {
        buf->cap = buf->len + total + 65536;
        char* nd = (char*)realloc(buf->data, buf->cap);
        if (!nd) return 0;
        buf->data = nd;
    }
    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* =========================================================================
 *  HTTP GET via libcurl
 * ========================================================================= */

static char* curl_get(const char* url)
{
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;

    CurlBuf buf = { NULL, 0, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CockpitSim/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_WARN("Weather: curl GET failed: %s", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    if (!buf.data) return NULL;
    return buf.data;  /* caller must free */
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

int weather_fetch_for_coords(double lat, double lon,
                             char* weather_out, size_t weather_sz,
                             float* temp_out, int* humidity_out)
{
    /* Init defaults */
    if (weather_out) weather_out[0] = '\0';
    if (temp_out) *temp_out = -999.0f;
    if (humidity_out) *humidity_out = -1;

    /* Build Open-Meteo URL — single call, no API key needed */
    char url[512];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?"
        "latitude=%.6f&longitude=%.6f"
        "&current=temperature_2m,relative_humidity_2m,weather_code"
        "&timezone=auto",
        lat, lon);

    char* json_str = curl_get(url);
    if (!json_str) return -1;

    cJSON* root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) return -1;

    int ok = -1;
    cJSON* current = cJSON_GetObjectItem(root, "current");
    if (current && cJSON_IsObject(current)) {
        cJSON* w = cJSON_GetObjectItem(current, "weather_code");
        cJSON* t = cJSON_GetObjectItem(current, "temperature_2m");
        cJSON* h = cJSON_GetObjectItem(current, "relative_humidity_2m");

        if (w && cJSON_IsNumber(w) && weather_out) {
            const char* desc = wmo_to_chinese(w->valueint);
            strncpy(weather_out, desc, weather_sz - 1);
            weather_out[weather_sz - 1] = '\0';
        }
        if (t && cJSON_IsNumber(t) && temp_out) {
            *temp_out = (float)t->valuedouble;
        }
        if (h && cJSON_IsNumber(h) && humidity_out) {
            *humidity_out = (int)h->valuedouble;
        }
        ok = 0;
    }

    cJSON_Delete(root);

    if (ok == 0) {
        LOG_INFO("Weather: %.4f,%.4f → %s %.1f°C %d%%",
                 lat, lon,
                 weather_out ? weather_out : "?",
                 temp_out ? (double)*temp_out : -999.0,
                 humidity_out ? *humidity_out : -1);
    } else {
        LOG_WARN("Weather: no current data for %.4f,%.4f", lat, lon);
    }

    return ok;
}
