/**
 * @file    map_display.h
 * @brief   Native SDL2 cabin trajectory display — separate window.
 *
 * Creates an independent SDL window for the cabin moving map.
 * Fetches 高德 map tiles via HTTP in a background thread.
 * Renders map, trajectory overlay, and data bar in the cabin window.
 *
 * Config: reads [map] section from default.cfg.
 */

#ifndef MAP_DISPLAY_H
#define MAP_DISPLAY_H

#include "data/flight_data.h"
#include "data/navdata.h"
#include "tile_cache.h"

#include <SDL2/SDL.h>

typedef struct Config Config;

typedef struct {
    char    city[64];
    char    weather[64];
    float   temp_c;
    float   wind_speed;
    char    wind_dir[16];
    int     humidity;
    char    report_time[32];
} WeatherInfo;

typedef struct MapDisplay {
    /* --- Config --- */
    char     api_key[64];
    int      zoom;
    int      tile_size;
    int      fetch_interval_ms;

    /* --- SDL window --- */
    SDL_Window*   window;
    SDL_Renderer* renderer;
    int           win_w, win_h;
    SDL_Rect      map_rect;

    /* --- Tile cache & fetch --- */
    TileCache*    tile_cache;
    SDL_Thread*   fetch_thread;
    SDL_atomic_t  fetch_running;
    SDL_atomic_t  fetch_pending;
    double        fetch_lat, fetch_lon;
    int           fetch_zoom;
    SDL_mutex*    fetch_mutex;

    /* --- Pending surfaces --- */
    #define CABIN_PENDING_MAX  9
    struct { SDL_Surface* surf; char key[32]; } pending[CABIN_PENDING_MAX];
    int           pending_count;
    SDL_mutex*    pending_mutex;

    /* --- Flight data --- */
    SDL_mutex*       data_mutex;
    FlightDataValues last_fd;

    /* --- Interpolated position --- */
    double        disp_lat, disp_lon;

    /* --- FMC --- */
    FMCState*     fmc;

    /* --- Auto-zoom --- */
    double        base_zoom;
    uint64_t      zoom_start_ms;

    /* --- Route change --- */
    int           last_wpt_count;
    char          last_dep_icao[8];
    char          last_arr_icao[8];

    /* --- Weather --- */
    WeatherInfo   weather;
    uint64_t      last_weather_fetch;
    int           weather_interval_s;
} MapDisplay;

MapDisplay* map_display_create(const Config* cfg, FMCState* fmc);
void        map_display_destroy(MapDisplay* md);
void        map_display_update_position(MapDisplay* md, const FlightDataValues* fd);
void        map_display_render(MapDisplay* md);

#endif
