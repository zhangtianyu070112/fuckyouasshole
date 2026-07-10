/**
 * @file    map_display.h
 * @brief   Cabin 3D globe display — separate OpenGL window.
 *
 * Creates an independent SDL window with OpenGL context for a 3D globe
 * cabin moving map. Loads an equirectangular earth texture, renders it
 * on a UV sphere, and draws flight route/track overlays in 3D.
 *
 * 2D elements (header, data bar, progress bar) are rendered via
 * orthographic projection on top.
 *
 * Config: reads [map] section from default.cfg.
 */

#ifndef MAP_DISPLAY_H
#define MAP_DISPLAY_H

#include "data/flight_data.h"
#include "data/navdata.h"

#include <SDL2/SDL.h>

/* Need OpenGL headers — SDL_opengl.h for GL, plus GLU */
#include <SDL2/SDL_opengl.h>
#include <GL/glu.h>
#include <SDL2/SDL_ttf.h>

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
    /* --- SDL window --- */
    SDL_Window*    window;
    SDL_GLContext  gl_ctx;
    int            win_w, win_h;

    /* --- OpenGL objects --- */
    GLuint         earth_tex;       /* equirectangular earth texture */
    GLuint         plane_tex;       /* aircraft icon sprite */
    int            plane_w, plane_h;/* sprite dimensions */
    GLuint         sphere_list;     /* display list for UV sphere mesh */
    int            sphere_lats;     /* mesh resolution */
    int            sphere_lons;

    /* --- Camera --- */
    float          camera_dist;     /* distance from globe center */
    float          globe_tilt;      /* tilt angle (degrees, ~25°) */
    float          globe_rot_y;     /* current Y rotation (degrees) */
    float          target_rot_y;    /* target Y rotation (interpolated) */

    /* --- Font for 2D overlay --- */
    TTF_Font*      font_small;
    TTF_Font*      font_large;
    TTF_Font*      font_bold;

    /* --- Flight data --- */
    SDL_mutex*       data_mutex;
    FlightDataValues last_fd;

    /* --- Interpolated position --- */
    double        disp_lat, disp_lon;

    /* --- GPS track history --- */
    #define CABIN_TRACK_MAX  2048
    struct { double lat; double lon; } track[CABIN_TRACK_MAX];
    int           track_count;
    uint64_t      last_track_add_ms;

    /* --- FMC --- */
    FMCState*     fmc;

    /* --- Auto-zoom (now orbit animation) --- */
    uint64_t      zoom_start_ms;

    /* --- Route change detection --- */
    int           last_wpt_count;
    char          last_dep_icao[8];
    char          last_arr_icao[8];

    /* --- Weather --- */
    WeatherInfo   weather_dep;
    WeatherInfo   weather_arr;
    uint64_t      last_weather_fetch_ms;
    SDL_atomic_t  weather_fetching;
} MapDisplay;

MapDisplay* map_display_create(const Config* cfg, FMCState* fmc);
void        map_display_destroy(MapDisplay* md);
void        map_display_update_position(MapDisplay* md, const FlightDataValues* fd);
void        map_display_render(MapDisplay* md);

#endif
