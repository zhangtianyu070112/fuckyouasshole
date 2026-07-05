/**
 * @file    map_display.c
 * @brief   Map display module implementation (stub for skeleton).
 *
 * Full implementation requires:
 *   1. HTTP GET to Amap weather API
 *   2. JSON parsing of the response
 *   3. Rendering map tiles (either from Amap static map API or local cache)
 *
 * This skeleton provides the structure and API stubs.
 */

#include "map_display.h"
#include "http.h"
#include "data/flight_data.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 *  Create / Destroy
 * ========================================================================= */

MapDisplay* map_display_create(const char* api_key, const char* city_code,
                               int fetch_interval_s)
{
    MapDisplay* md = calloc(1, sizeof(MapDisplay));
    if (!md) {
        LOG_ERROR("Out of memory allocating MapDisplay");
        return NULL;
    }

    if (api_key)    strncpy(md->api_key, api_key, sizeof(md->api_key) - 1);
    if (city_code)  strncpy(md->city_code, city_code, sizeof(md->city_code) - 1);
    md->fetch_interval_s = fetch_interval_s > 0 ? fetch_interval_s : 300;  /* 5 min default */

    LOG_INFO("MapDisplay created (api_key=%s..., city=%s, interval=%ds)",
             md->api_key[0] ? "SET" : "NONE",
             md->city_code[0] ? md->city_code : "NONE",
             md->fetch_interval_s);

    return md;
}

void map_display_destroy(MapDisplay* md)
{
    if (md) free(md);
}

/* =========================================================================
 *  Fetch weather
 * ========================================================================= */

int map_display_fetch_weather(MapDisplay* md)
{
    if (!md || md->api_key[0] == '\0' || md->city_code[0] == '\0') {
        LOG_WARN("Map: cannot fetch weather — missing API key or city code");
        return -1;
    }

    /* Build Amap weather API URL */
    char path[512];
    snprintf(path, sizeof(path),
             "/v3/weather/weatherInfo?key=%s&city=%s&extensions=base&output=JSON",
             md->api_key, md->city_code);

    LOG_DEBUG("Map: fetching weather from Amap API...");

    HTTPResponse* resp = http_get("restapi.amap.com", 80, path);
    if (!resp || resp->status_code != 200) {
        LOG_WARN("Map: weather fetch failed (status=%d, err=%s)",
                 resp ? resp->status_code : 0,
                 resp ? resp->error_msg : "no response");
        if (resp) http_response_free(resp);
        return -1;
    }

    /* TODO: Parse JSON response body.
     * For now, store the raw response and let the rendering layer extract
     * what it needs. In production, use a JSON parser (cJSON, jsmn, etc.)
     */
    LOG_INFO("Map: weather fetched (%zu bytes)", resp->body_len);

    md->weather.temp_c = 25.0f;  /* Placeholder — replace with JSON parse */
    md->last_fetch_ticks = SDL_GetTicks();

    http_response_free(resp);
    return 0;
}

/* =========================================================================
 *  Render (stub)
 * ========================================================================= */

void map_display_render(MapDisplay* md, void* sdl_renderer,
                        const FlightDataValues* flight_data)
{
    (void)md;
    (void)sdl_renderer;
    (void)flight_data;

    /* TODO: Full map rendering implementation.
     *
     * Approach:
     *   1. Determine map center from flight_data->lat_deg / lon_deg
     *   2. Compute Mercator projection tile coordinates
     *   3. Fetch map tiles from Amap static tile API:
     *      https://webrd0{1-4}.is.autonavi.com/appmaptile?x={x}&y={y}&z={z}&lang=zh_cn
     *   4. Render tiles as SDL textures with SDL_RenderCopy
     *   5. Overlay aircraft symbol, track line, waypoints from FMC
     *
     * For the skeleton, this renders nothing.
     */
}
