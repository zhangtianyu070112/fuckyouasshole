/**
 * @file    weather_fetch.c
 * @brief   Fetch weather from 高德 REST APIs via libcurl + cJSON.
 */

#include "weather_fetch.h"
#include "utils/logger.h"

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
 *  Reverse geocode → adcode
 * ========================================================================= */

static char* regeo_get_adcode(const char* api_key, double lon, double lat)
{
    char url[512];
    snprintf(url, sizeof(url),
        "https://restapi.amap.com/v3/geocode/regeo?"
        "key=%s&location=%.6f,%.6f&output=JSON",
        api_key, lon, lat);

    char* json_str = curl_get(url);
    if (!json_str) return NULL;

    cJSON* root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) return NULL;

    char* adcode = NULL;
    cJSON* regeo = cJSON_GetObjectItem(root, "regeocode");
    if (regeo) {
        cJSON* comp = cJSON_GetObjectItem(regeo, "addressComponent");
        if (comp) {
            cJSON* ac = cJSON_GetObjectItem(comp, "adcode");
            if (ac && cJSON_IsString(ac)) {
                adcode = strdup(ac->valuestring);
            }
        }
    }
    cJSON_Delete(root);
    return adcode;
}

/* =========================================================================
 *  Weather query
 * ========================================================================= */

static int weather_query(const char* api_key, const char* adcode,
                         char* weather_out, size_t weather_sz,
                         float* temp_out, int* humidity_out)
{
    char url[512];
    snprintf(url, sizeof(url),
        "https://restapi.amap.com/v3/weather/weatherInfo?"
        "key=%s&city=%s&extensions=base&output=JSON",
        api_key, adcode);

    char* json_str = curl_get(url);
    if (!json_str) return -1;

    cJSON* root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) return -1;

    int ok = -1;
    cJSON* lives = cJSON_GetObjectItem(root, "lives");
    if (lives && cJSON_IsArray(lives)) {
        cJSON* live = cJSON_GetArrayItem(lives, 0);
        if (live) {
            cJSON* w = cJSON_GetObjectItem(live, "weather");
            cJSON* t = cJSON_GetObjectItem(live, "temperature");
            cJSON* h = cJSON_GetObjectItem(live, "humidity");

            if (w && cJSON_IsString(w) && weather_out) {
                strncpy(weather_out, w->valuestring, weather_sz - 1);
                weather_out[weather_sz - 1] = '\0';
            }
            if (t && cJSON_IsString(t) && temp_out) {
                *temp_out = (float)atof(t->valuestring);
            }
            if (h && cJSON_IsString(h) && humidity_out) {
                *humidity_out = atoi(h->valuestring);
            }
            ok = 0;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

int weather_fetch_for_coords(const char* api_key,
                             double lat, double lon,
                             char* weather_out, size_t weather_sz,
                             float* temp_out, int* humidity_out)
{
    if (!api_key || !api_key[0]) return -1;

    if (weather_out) weather_out[0] = '\0';
    if (temp_out) *temp_out = -999.0f;
    if (humidity_out) *humidity_out = -1;

    /* Step 1: reverse geocode → adcode */
    char* adcode = regeo_get_adcode(api_key, lon, lat);
    if (!adcode) {
        LOG_WARN("Weather: regeo failed for %.4f,%.4f", lat, lon);
        return -1;
    }
    LOG_DEBUG("Weather: regeo %.4f,%.4f → adcode=%s", lat, lon, adcode);

    /* Step 2: weather query */
    int ret = weather_query(api_key, adcode, weather_out, weather_sz,
                            temp_out, humidity_out);
    if (ret == 0) {
        LOG_INFO("Weather: %s %.1f°C %d%% (adcode=%s)",
                 weather_out ? weather_out : "?",
                 temp_out ? (double)*temp_out : -999.0,
                 humidity_out ? *humidity_out : -1,
                 adcode);
    }
    free(adcode);
    return ret;
}
