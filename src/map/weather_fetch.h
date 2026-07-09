/**
 * @file    weather_fetch.h
 * @brief   Fetch weather data from 高德 REST API via libcurl + cJSON.
 *
 * Flow:
 *   1. Reverse geocode lat/lon → adcode (city code)
 *   2. Query weather API with adcode → weather description + temperature
 *
 * Uses libcurl for HTTPS and cJSON for JSON parsing.
 */

#ifndef WEATHER_FETCH_H
#define WEATHER_FETCH_H

#include <stddef.h>

/**
 * @brief Fetch current weather for a geographic location.
 *
 * Calls the 高德逆地理编码 API to get the city adcode, then the 高德天气
 * API to get current conditions. Blocks for the duration of the HTTP calls
 * (~1-3 seconds total). Call from a background thread.
 *
 * @param api_key    高德 Web API key.
 * @param lat        Latitude in decimal degrees.
 * @param lon        Longitude in decimal degrees.
 * @param weather_out  Buffer for weather description (e.g. "晴", "多云").
 *                     Must be at least 64 bytes.
 * @param temp_out   [out] Temperature in Celsius, or -999 on failure.
 * @param humidity_out [out] Humidity percentage, or -1 on failure.
 * @return 0 on success, -1 on failure.
 */
int weather_fetch_for_coords(const char* api_key,
                             double lat, double lon,
                             char* weather_out, size_t weather_sz,
                             float* temp_out, int* humidity_out);

#endif /* WEATHER_FETCH_H */
