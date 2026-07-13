/**
 * @file    app.h
 * @brief   Application framework — owns the SDL window, renderer, event loop,
 *          and all subsystems (config, instruments, flight data, network).
 *
 * Usage:
 *   int app_run_with_config("config/default.cfg");
 *
 * This is the only function main() needs to call. Everything else (SDL init,
 * instrument creation, main loop, shutdown) is handled internally.
 */

#ifndef APP_H
#define APP_H

#include <SDL2/SDL.h>

/* Forward declarations for owned subsystems */
typedef struct Config      Config;
typedef struct EventSys    EventSys;
typedef struct Instrument  Instrument;
typedef struct FlightData  FlightData;
typedef struct Thread      Thread;
typedef struct FMCState    FMCState;
typedef struct MockDataCtx MockDataCtx;
typedef struct AlertSystem AlertSystem;
typedef struct UDPSocket   UDPSocket;
typedef struct MapDisplay  MapDisplay;
typedef struct AIAdvisor   AIAdvisor;
typedef struct DepartureDB  DepartureDB;

/**
 * @brief Top-level application state.
 *
 * Created in app_init(), destroyed in app_shutdown().
 * Accessible by instruments via the App* pointer passed to on_init.
 */
typedef struct App {
    /* SDL */
    SDL_Window*    window;
    SDL_Renderer*  renderer;
    int            screen_w;
    int            screen_h;
    int            running;          /* Set to 0 to exit main loop */

    /* Timing */
    Uint32         last_frame_ticks;
    float          delta_time;       /* Seconds since last frame */
    float          target_frame_time;/* 1.0 / fps */

    /* Subsystems */
    Config*        config;
    EventSys*      events;
    FlightData*    flight_data;

    /* Instruments (array, NULL-terminated or count-tracked) */
    Instrument*    instruments[8];
    int            instrument_count;

    int            zoomed_instrument_index; /* -1 if none is zoomed, else 0..count-1 */
    SDL_Rect       instrument_base_rects[8]; /* The small rects on the cockpit bg */
    SDL_Texture*   instrument_targets[8];    /* Render targets for adaptive scaling */

    SDL_Texture*   bg_texture;
    SDL_Texture*   btn_full_screen;
    SDL_Texture*   btn_sub;

    /* Camera zoom/pan — active only when no instrument is fullscreened */
    float          cam_zoom;
    float          cam_quality;     /* internal render scale for scene_texture */
    float          cam_offset_x;
    float          cam_offset_y;
    int            cam_dragging;
    int            cam_drag_start_x;
    int            cam_drag_start_y;
    float          cam_drag_offset_x;
    float          cam_drag_offset_y;
    SDL_Texture*   scene_texture;

    /* Network / data source threads */
    Thread*        udp_thread;
    Thread*        mock_thread;
    MockDataCtx*   mock_ctx;

    /* FMC shared state (accessible by FMC instrument and data layer) */
    FMCState*      fmc_state;

    /* Departure procedure database (SID/runway/transition) */
    DepartureDB*   dep_db;

    /* GPWS audio alert system */
    AlertSystem*   alert_sys;

    /* X-Plane send socket (for DREF reverse communication, port 49000) */
    UDPSocket*     xp_send_sock;
    char           xp_host[64];
    int            xp_send_port;

    /* Cabin moving map display (native SDL2) */
    MapDisplay*    map_display;

    /* AI Co-pilot advisor (WebSocket to inference server) */
    AIAdvisor*     ai_advisor;

} App;

/* --- Public API -------------------------------------------------------- */

/**
 * @brief Run the full application lifecycle with the given config file.
 * @param config_path  Path to configuration file.
 * @return 0 on clean exit, non-zero on error.
 */
int app_run_with_config(const char* config_path);

/**
 * @brief Sync FMC flight plan to X-Plane autopilot via native UDP DREF.
 *
 * Sends cruise altitude, speed, and first-leg heading to X-Plane.
 * Safe to call when xp_send_sock is NULL — silently no-ops.
 */
void xplane_fmc_sync(const App* app);

#endif /* APP_H */
