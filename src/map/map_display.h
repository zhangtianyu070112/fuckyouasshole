/**
 * @file    map_display.h
 * @brief   External map/weather information display module.
 *
 * Integrates with Amap (高德地图) Web API to fetch and display:
 *   - Current flight position on map (via IP location or GPS coordinates)
 *   - Weather information at destination/alternate airports
 *   - Flight path overlay
 *
 * This module runs HTTP requests in a background thread (not implemented
 * in skeleton) to avoid blocking the render loop.
 *
 * API docs: https://lbs.amap.com/api/webservice/guide/api/
 */

#ifndef MAP_DISPLAY_H
#define MAP_DISPLAY_H

#include <stdint.h>

/* Forward declarations */
typedef struct FlightDataValues FlightDataValues;

/* --- Types ------------------------------------------------------------- */

typedef struct {
    char    city[64];         /* City name */
    char    weather[64];      /* Weather description */
    float   temp_c;           /* Temperature (°C) */
    float   wind_speed;       /* Wind speed */
    char    wind_dir[16];     /* Wind direction text */
    int     humidity;         /* Humidity % */
    char    report_time[32];  /* Report timestamp */
} WeatherInfo;

typedef struct {
    WeatherInfo weather;
    uint64_t    last_fetch_ticks;
    int         fetch_interval_s;  /* Seconds between API calls */
    char        api_key[64];       /* Amap API key */
    char        city_code[16];     /* City adcode */
} MapDisplay;

/* --- API --------------------------------------------------------------- */

/**
 * @brief Initialize the map display module.
 * @param api_key        Amap Web API key (from config).
 * @param city_code      Default city code (adcode) for weather queries.
 * @param fetch_interval_s  Interval between API fetches.
 */
MapDisplay* map_display_create(const char* api_key, const char* city_code,
                               int fetch_interval_s);

/**
 * @brief Free resources.
 */
void map_display_destroy(MapDisplay* md);

/**
 * @brief Attempt to fetch latest weather data (blocking).
 *        Call from a worker thread.
 * @return 0 on success, -1 on failure.
 */
int map_display_fetch_weather(MapDisplay* md);

/**
 * @brief Render map and weather info to the given SDL renderer.
 *        (Placeholder in skeleton — full implementation draws Amap tiles.)
 */
void map_display_render(MapDisplay* md, void* sdl_renderer,
                        const FlightDataValues* flight_data);

#endif /* MAP_DISPLAY_H */
