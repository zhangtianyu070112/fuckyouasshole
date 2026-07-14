/**
 * @file    weather_fetch.h
 * @brief   Fetch weather data from Open-Meteo free API via libcurl + cJSON.
 *
 * Open-Meteo provides global weather data without an API key.
 * Single HTTP GET by lat/lon returns temperature, humidity, and weather code.
 *
 * Uses libcurl for HTTPS and cJSON for JSON parsing.
 */

#ifndef WEATHER_FETCH_H
#define WEATHER_FETCH_H

#include <stddef.h>

/**
 * @brief Fetch current weather for a geographic location.
 *
 * Calls the Open-Meteo forecast API to get current conditions.
 * Blocks for the duration of the HTTP call (~0.5-2 seconds).
 * Call from a background thread.
 *
 * @param lat         Latitude in decimal degrees.
 * @param lon         Longitude in decimal degrees.
 * @param weather_out Buffer for Chinese weather description (e.g. "晴", "多云").
 *                    Must be at least 64 bytes.
 * @param weather_sz  Size of weather_out buffer.
 * @param temp_out    [out] Temperature in Celsius, or -999 on failure.
 * @param humidity_out [out] Relative humidity percentage, or -1 on failure.
 * @return 0 on success, -1 on failure.
 */
int weather_fetch_for_coords(double lat, double lon,
                             char* weather_out, size_t weather_sz,
                             float* temp_out, int* humidity_out);

#endif /* WEATHER_FETCH_H */
