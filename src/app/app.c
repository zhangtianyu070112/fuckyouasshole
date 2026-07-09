/**
 * @file    app.c
 * @brief   Application framework implementation.
 *
 * Manages the full application lifecycle:
 *   1. Parse config & init logging
 *   2. Init SDL (video, events, fonts)
 *   3. Create window & renderer
 *   4. Create subsystems (flight data, event dispatch, instruments)
 *   5. Start background threads (UDP)
 *   6. Main loop: pump events → update → render → cap framerate
 *   7. Shutdown: reverse order with resource cleanup
 */

#include "app.h"
#include "config.h"
#include "event.h"
#include "thread.h"
#include "utils/logger.h"
#include "instrument.h"
#include "EICAS/eicas2.h"
#include "data/flight_data.h"
#include "data/navdata.h"
#include "ds/spatial_hash.h"
#include "net/udp.h"
#include "net/xplane.h"
#include "net/mock_data.h"
#include "audio/alert_system.h"
#include "utils/font_manager.h"
#include "map/map_display.h"

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

/* Instrument constructors (defined in their respective .c files) */
extern Instrument* pfd_create(void);
extern Instrument* nd_create(void);
extern Instrument* eicas_create(void);
extern Instrument* fmc_create(void);
extern FMCState*   fmc_state_create(void);
extern void        fmc_state_free(FMCState* state);

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 *  Internal helpers
 * ========================================================================= */

/**
 * @brief Initialize SDL2 and its extension libraries.
 * @return 0 on success, -1 on failure.
 */
static int init_sdl(App* app)
{
    Uint32 flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_AUDIO;
    if (SDL_Init(flags) != 0) {
        fprintf(stderr, "FATAL: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Determine window flags from config */
    int fullscreen = config_get_bool(app->config, "window", "fullscreen", 0);
    Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (fullscreen) {
        win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    /* Enable linear scaling for better texture/font resizing */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    const char* title = config_get_str(app->config, "window", "title",
                                       "Flight Cockpit Simulation System");

    app->screen_w = (int)config_get_int(app->config, "window", "width", 1920);
    app->screen_h = (int)config_get_int(app->config, "window", "height", 1080);

    app->window = SDL_CreateWindow(title,
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    app->screen_w, app->screen_h,
                                    win_flags);
    if (!app->window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return -1;
    }

    /* Read actual window size — fullscreen desktop may override config values */
    {
        int real_w, real_h;
        SDL_GetWindowSize(app->window, &real_w, &real_h);
        if (real_w != app->screen_w || real_h != app->screen_h) {
            LOG_INFO("Window actual size: %dx%d (config was %dx%d)",
                     real_w, real_h, app->screen_w, app->screen_h);
            app->screen_w = real_w;
            app->screen_h = real_h;
        }
    }

    /* Create accelerated renderer with vsync */
    int vsync = config_get_bool(app->config, "render", "vsync", 1);
    app->renderer = SDL_CreateRenderer(app->window, -1,
                                        SDL_RENDERER_ACCELERATED |
                                        SDL_RENDERER_TARGETTEXTURE |
                                        (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!app->renderer) {
        LOG_WARN("Accelerated renderer failed, trying software: %s", SDL_GetError());
        app->renderer = SDL_CreateRenderer(app->window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
        if (!app->renderer) {
            LOG_ERROR("SDL_CreateRenderer failed: %s", SDL_GetError());
            return -1;
        }
    }

    /* Enable alpha blending by default */
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);

    /* Font init — TTF is optional; instruments should handle NULL font gracefully */
    if (TTF_Init() != 0) {
        LOG_WARN("TTF_Init failed (fonts disabled): %s", TTF_GetError());
    } else {
        if (font_system_init("resources/fonts") != 0) {
            LOG_WARN("Font system init failed — text will be placeholder crosses");
        }
    }

    /* Load background and UI textures */
    app->zoomed_instrument_index = -1;
    SDL_Surface* surf = IMG_Load("assets/assets/main.png");
    if (surf) {
        app->bg_texture = SDL_CreateTextureFromSurface(app->renderer, surf);
        SDL_FreeSurface(surf);
    }
    surf = IMG_Load("assets/assets/full_screen.png");
    if (surf) {
        app->btn_full_screen = SDL_CreateTextureFromSurface(app->renderer, surf);
        SDL_FreeSurface(surf);
    }
    surf = IMG_Load("assets/assets/sub.png");
    if (surf) {
        app->btn_sub = SDL_CreateTextureFromSurface(app->renderer, surf);
        SDL_FreeSurface(surf);
    }

    LOG_INFO("SDL initialized: %dx%d window", app->screen_w, app->screen_h);
    return 0;
}

/**
 * @brief Layout instruments to match the cockpit background image (main.png).
 */
static void layout_instruments(App* app)
{
    int count = app->instrument_count;
    if (count == 0) return;

    /* Base coordinates of the 8 screens in the 8026x3136 background image */
    /* [0] CAPT PFD, [1] EICAS1, [2] FO PFD, [3] CAPT ND, [4] LEFT FMC, [5] EICAS2, [6] FO ND, [7] RIGHT FMC */
    SDL_Rect orig_rects[8];
    /* CAPT PFD */
    orig_rects[0] = (SDL_Rect){1255, 906, 828, 778};
    /* EICAS 1 (Upper) */
    orig_rects[1] = (SDL_Rect){3570, 915, 830, 779};
    /* FO PFD */
    orig_rects[2] = (SDL_Rect){5664, 906, 829, 778};
    /* CAPT ND */
    orig_rects[3] = (SDL_Rect){2165, 906, 830, 778};
    /* LEFT FMC */
    orig_rects[4] = (SDL_Rect){2766, 1806, 643, 992};
    /* EICAS 2 (Lower) */
    orig_rects[5] = (SDL_Rect){3505, 1891, 830, 779};
    /* FO ND */
    orig_rects[6] = (SDL_Rect){4745, 906, 830, 778};
    /* RIGHT FMC */
    orig_rects[7] = (SDL_Rect){4617, 1806, 643, 992};

    /* Background image dimensions */
    float bg_w = 8026.0f;
    float bg_h = 3136.0f;

    /* Calculate the scale and offset to fit the background image in the window while maintaining aspect ratio */
    float scale_x = (float)app->screen_w / bg_w;
    float scale_y = (float)app->screen_h / bg_h;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;

    int render_w = (int)(bg_w * scale);
    int render_h = (int)(bg_h * scale);
    int offset_x = (app->screen_w - render_w) / 2;
    int offset_y = (app->screen_h - render_h) / 2;

    for (int i = 0; i < count && i < 8; i++) {
        app->instrument_base_rects[i].x = offset_x + (int)(orig_rects[i].x * scale);
        app->instrument_base_rects[i].y = offset_y + (int)(orig_rects[i].y * scale);
        app->instrument_base_rects[i].w = (int)(orig_rects[i].w * scale);
        app->instrument_base_rects[i].h = (int)(orig_rects[i].h * scale);

        /* Set internal rect strictly to 772x721 for most instruments. 
         * For FMC, we use its native proportions (643x992).
         * We will use SDL_RenderSetScale to dynamically fit it into the base rects. */
        int native_w = 772;
        int native_h = 721;
        if (app->instruments[i]->name && strstr(app->instruments[i]->name, "FMC")) {
            native_w = 643;
            native_h = 992;
        }

        app->instruments[i]->rect.w = native_w;
        app->instruments[i]->rect.h = native_h;
        app->instruments[i]->rect.x = 0;
        app->instruments[i]->rect.y = 0;
        
        if (!app->instrument_targets[i]) {
            app->instrument_targets[i] = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, native_w, native_h);
            if (app->instrument_targets[i]) {
                SDL_SetTextureBlendMode(app->instrument_targets[i], SDL_BLENDMODE_BLEND);
            } else {
                LOG_ERROR("Failed to create target texture for instrument %d", i);
            }
        }

        app->instruments[i]->needs_redraw = 1;
    }

    LOG_INFO("Instruments layout calculated for %dx%d window", app->screen_w, app->screen_h);
}

/**
 * @brief Create instruments — dual-pilot layout (Captain + First Officer).
 *
 * Order determines layout position (3×2 grid, row-major):
 *   [0] CAPT PFD  [1] EICAS      [2] FO PFD
 *   [3] CAPT ND   [4] FMC        [5] FO ND
 */
static int create_instruments(App* app)
{
    int idx = 0;

    /* Row 0 — Captain PFD (left seat) */
    if (config_get_bool(app->config, "instruments", "pfd_enabled", 1)) {
        app->instruments[idx] = pfd_create();
        if (app->instruments[idx]) {
            app->instruments[idx]->name = "CAPT PFD";
            idx++;
        }
    }

    /* Row 0 — EICAS (center, shared) */
    if (config_get_bool(app->config, "instruments", "eicas_enabled", 1)) {
        app->instruments[idx] = eicas_create();
        if (app->instruments[idx]) { idx++; }
    }

    /* Row 0 — First Officer PFD (right seat) */
    if (config_get_bool(app->config, "instruments", "pfd_enabled", 1)) {
        app->instruments[idx] = pfd_create();
        if (app->instruments[idx]) {
            app->instruments[idx]->name = "FO PFD";
            idx++;
        }
    }

    /* Row 1 — Captain ND (left seat) */
    if (config_get_bool(app->config, "instruments", "nd_enabled", 1)) {
        app->instruments[idx] = nd_create();
        if (app->instruments[idx]) {
            app->instruments[idx]->name = "CAPT ND";
            idx++;
        }
    }

    /* Row 1 — FMC/CDU (center left) */
    if (config_get_bool(app->config, "instruments", "fmc_enabled", 1)) {
        app->instruments[idx] = fmc_create();
        if (app->instruments[idx]) { 
            app->instruments[idx]->name = "LEFT FMC";
            idx++; 
        }
    }

    /* Row 1 — EICAS2 (center bottom, shared) */
    if (config_get_bool(app->config, "instruments", "eicas2_enabled", 1)) {
        app->instruments[idx] = eicas2_create();
        if (app->instruments[idx]) { 
            app->instruments[idx]->name = "EICAS2";
            idx++; 
        }
    }

    /* Row 1 — First Officer ND (right seat) */
    if (config_get_bool(app->config, "instruments", "nd_enabled", 1)) {
        app->instruments[idx] = nd_create();
        if (app->instruments[idx]) {
            app->instruments[idx]->name = "FO ND";
            idx++;
        }
    }

    /* Row 1 — Right FMC/CDU (center right) */
    if (config_get_bool(app->config, "instruments", "fmc_enabled", 1)) {
        app->instruments[idx] = fmc_create();
        if (app->instruments[idx]) {
            app->instruments[idx]->name = "RIGHT FMC";
            idx++;
        }
    }

    app->instrument_count = idx;
    LOG_INFO("Created %d instrument(s) (dual-pilot layout)", idx);
    return idx;
}

/**
 * @brief Initialize all instruments (call on_init after layout).
 */
static void init_instruments(App* app)
{
    for (int i = 0; i < app->instrument_count; i++) {
        if (app->instruments[i] && app->instruments[i]->on_init) {
            app->instruments[i]->on_init(app->instruments[i], app);
        }
    }

    LOG_INFO("All instruments initialized");
}

/**
 * @brief Destroy all instruments.
 */
static void destroy_instruments(App* app)
{
    for (int i = 0; i < app->instrument_count; i++) {
        if (app->instruments[i]) {
            if (app->instruments[i]->on_destroy) {
                app->instruments[i]->on_destroy(app->instruments[i]);
            }
            free(app->instruments[i]);
            app->instruments[i] = NULL;
        }
        if (app->instrument_targets[i]) {
            SDL_DestroyTexture(app->instrument_targets[i]);
            app->instrument_targets[i] = NULL;
        }
    }
    app->instrument_count = 0;
}

/* =========================================================================
 *  UDP network thread
 * ========================================================================= */

/**
 * @brief Data passed to the UDP thread worker.
 */
typedef struct {
    App*        app;
    const char* xplane_host;
    int         xplane_cmd_port;   /* X-Plane's command-receive port (default 49000) */
    int         recv_port;         /* Our local receive port for DATA packets */
    int         data_rate;
} UDPThreadArgs;

/**
 * @brief UDP thread worker function.
 *
 * Opens a UDP socket, sends an RPOS subscription to X-Plane,
 * then loops receiving and parsing DATA packets into flight_data.
 */
static int udp_thread_worker(void* userdata)
{
    UDPThreadArgs* args = (UDPThreadArgs*)userdata;
    App* app = args->app;

    /* Open UDP socket for receiving X-Plane data — bind to our receive port */
    UDPSocket* sock = udp_socket_create(args->recv_port);
    if (!sock) {
        LOG_ERROR("UDP thread: failed to bind port %d", args->recv_port);
        free(args);
        return -1;
    }

    /* Send subscription request to X-Plane's command port */
    xplane_subscribe(sock, args->xplane_host, args->xplane_cmd_port, args->data_rate);

    LOG_INFO("UDP thread: listening on port %d, requesting data at %d Hz from %s:%d",
             args->recv_port, args->data_rate, args->xplane_host, args->xplane_cmd_port);

    /* Receive loop — UDP max datagram is 65535 bytes */
    uint8_t buf[65536];
    int consec_errors = 0;
    while (app->running && !thread_should_stop(app->udp_thread)) {
        int n = udp_socket_recv(sock, buf, sizeof(buf), 100);  /* 100ms timeout */
        if (n < 0) {
            consec_errors++;
            if (consec_errors == 1) {
                LOG_WARN("UDP recv error — check firewall / port binding");
            }
            continue;
        }
        if (n == 0) {
            consec_errors = 0;
            continue;  /* Timeout */
        }
        consec_errors = 0;

        /* First-packet milestone: confirms X-Plane is sending to our port */
        static int first_packet = 1;
        if (first_packet) {
            first_packet = 0;
            LOG_INFO("🎯 First UDP packet received! len=%d, hex=%02X %02X %02X %02X %02X...",
                     n, buf[0], buf[1], buf[2], buf[3], buf[4]);
        }

        xplane_parse_packet(buf, n, app->flight_data);

        /* Periodic throughput log */
        static int pkt_total = 0;
        pkt_total++;
        if (pkt_total % 1000 == 0) {
            LOG_INFO("📡 %d packets received so far", pkt_total);
        }
    }

    udp_socket_destroy(sock);
    free(args);
    LOG_INFO("UDP thread: exiting");
    return 0;
}

/**
 * @brief Start the data source thread — either mock generator or UDP.
 */
static int start_data_source(App* app)
{
    const char* source = config_get_str(app->config, "network", "data_source", "xplane");
    int data_rate      = (int)config_get_int(app->config, "network", "data_rate", 20);

    if (source && strcmp(source, "mock") == 0) {
        /* --- Mock data generator --- */
        app->mock_ctx = mock_data_create(app->flight_data, data_rate);
        if (!app->mock_ctx) {
            LOG_ERROR("Failed to create mock data context");
            return -1;
        }
        app->mock_thread = thread_create("MockData", mock_data_thread, app->mock_ctx);
        if (!app->mock_thread) {
            LOG_ERROR("Failed to start mock data thread");
            mock_data_free(app->mock_ctx);
            app->mock_ctx = NULL;
            return -1;
        }
        LOG_INFO("Mock data generator started at %d Hz", data_rate);
        return 0;
    }

    /* --- X-Plane UDP (default) --- */
    const char* host  = config_get_str(app->config, "network", "xplane_host", NULL);
    int cmd_port      = (int)config_get_int(app->config, "network", "xplane_cmd_port", 49000);
    int recv_port     = (int)config_get_int(app->config, "network", "udp_recv_port", 49000);

/* --- X-Plane host/port stored in App for DREF send access --------- */
    app->xp_send_sock = NULL;
    app->xp_host[0]   = '\0';
    app->xp_send_port = cmd_port;  /* DREF/CMND go to X-Plane's command port */

    if (!host || host[0] == '\0') {
        LOG_INFO("No xplane_host configured — falling back to mock data");
        app->mock_ctx = mock_data_create(app->flight_data, data_rate);
        if (!app->mock_ctx) return -1;
        app->mock_thread = thread_create("MockData", mock_data_thread, app->mock_ctx);
        if (!app->mock_thread) {
            mock_data_free(app->mock_ctx);
            app->mock_ctx = NULL;
            return -1;
        }
        LOG_INFO("Mock data generator started (fallback) at %d Hz", data_rate);
        return 0;
    }

    UDPThreadArgs* args = malloc(sizeof(UDPThreadArgs));
    if (!args) {
        LOG_ERROR("Out of memory for UDP thread args");
        return -1;
    }
    args->app              = app;
    args->xplane_host       = host;
    args->xplane_cmd_port   = cmd_port;
    args->recv_port         = recv_port;
    args->data_rate         = data_rate;

    app->udp_thread = thread_create("UDP-Recv", udp_thread_worker, args);
    if (!app->udp_thread) {
        LOG_ERROR("Failed to start UDP thread");
        free(args);
        return -1;
    }

    /* Create a send-only UDP socket for DREF reverse communication.
     * This is separate from the recv thread's socket — bind to port 0 (OS picks).
     * DREF/CMND packets go to X-Plane's command port (same port we send RPOS/DSEL to). */
    app->xp_send_sock = udp_socket_create(0);
    if (app->xp_send_sock) {
        strncpy(app->xp_host, host, sizeof(app->xp_host) - 1);
        app->xp_send_port = cmd_port;   /* DREF → X-Plane command port */
        LOG_INFO("X-Plane send socket ready → %s:%d", host, app->xp_send_port);

        /* Subscribe to alert-state DREFs via RREF (Route A).
         * Data comes back on the receive port (recv_port), parsed by
         * xplane_parse_rref() in the UDP receive thread. */
        xplane_rref_subscribe_all(app->xp_send_sock, host, cmd_port);

        /* Subscribe to ND-specific DREFs (position, heading, speed) */
        xplane_rref_subscribe_nd(app->xp_send_sock, host, cmd_port);
    } else {
        LOG_WARN("Failed to create X-Plane send socket — DREF disabled");
    }

    return 0;
}

/* =========================================================================
 *  FMC → X-Plane reverse sync
 * ========================================================================= */

/**
 * @brief Sync FMC flight plan parameters back to X-Plane via native UDP DREF.
 *
 * Call this AFTER a route has been activated (A* search completed and
 * FlightPlan populated). Sends autopilot altitude, speed, and first-leg
 * heading to X-Plane so the aircraft can follow the FMC route.
 *
 * @param app  Application state (needs xp_send_sock to be valid).
 */
void xplane_fmc_sync(const App* app)
{
    if (!app || !app->xp_send_sock || !app->fmc_state) return;
    if (app->xp_host[0] == '\0') return;

    const FlightPlan* fp = &app->fmc_state->flight_plan;
    if (fp->waypoint_count < 2) return;

    int port = app->xp_send_port;
    const char* host = app->xp_host;

    /* 1. Cruise altitude → autopilot altitude window */
    if (fp->cruise_altitude_ft > 100.0f) {
        xplane_send_dref(app->xp_send_sock, host, port,
            "sim/cockpit/autopilot/altitude", fp->cruise_altitude_ft);
        SDL_Delay(15);  /* prevent XP12 UDP packet collision on port 49000 */
    }

    /* 2. Cruise speed (TAS) → autopilot airspeed (IAS knot, close enough) */
    if (fp->cruise_speed_kts > 50.0f) {
        xplane_send_dref(app->xp_send_sock, host, port,
            "sim/cockpit/autopilot/airspeed", fp->cruise_speed_kts);
        SDL_Delay(15);
    }

    /* 3. First leg bearing → autopilot heading */
    {
        const Waypoint* w0 = &fp->waypoints[0];
        const Waypoint* w1 = &fp->waypoints[1];
        float bearing = (float)geo_bearing_deg(w0->pos, w1->pos);
        xplane_send_dref(app->xp_send_sock, host, port,
            "sim/cockpit/autopilot/heading_mag", bearing);
    }

    LOG_INFO("FMC→XP sync: ALT %.0f ft, SPD %.0f kt, HDG from %s→%s",
             (double)fp->cruise_altitude_ft,
             (double)fp->cruise_speed_kts,
             fp->waypoints[0].ident, fp->waypoints[1].ident);
}

static void main_loop(App* app)
{
    app->last_frame_ticks = SDL_GetTicks();
    float fps = (float)config_get_int(app->config, "render", "target_fps", 60);
    app->target_frame_time = (fps > 0.0f) ? (1.0f / fps) : 0.0f;

    LOG_INFO("Entering main loop (target %.0f FPS)", fps);

    while (app->running) {
        Uint32 frame_start = SDL_GetTicks();

        /* 1. Calculate delta time */
        app->delta_time = (float)(frame_start - app->last_frame_ticks) / 1000.0f;
        app->last_frame_ticks = frame_start;

        /* 2. Pump events */
        eventsys_pump(app->events);

        /* Check for window resize to relayout instruments */
        int w, h;
        SDL_GetWindowSize(app->window, &w, &h);
        if (w != app->screen_w || h != app->screen_h) {
            app->screen_w = w;
            app->screen_h = h;
            layout_instruments(app);
        }

        /* 3. Read shared flight data (lock briefly) */
        FlightDataValues snapshot;
        flight_data_snapshot(app->flight_data, &snapshot);

        /* Evaluate triggers & play audio based on flight data */
        if (app->alert_sys) {
            alert_system_update(app->alert_sys, &snapshot, app->delta_time);
            /* alert_system_test_beeps(app->alert_sys, &snapshot); */
        }

        /* 3c. Update & render Cabin Map Display */
        if (app->map_display) {
            map_display_update_position(app->map_display, &snapshot);
            map_display_render(app->map_display);
        }

        /* 4. Update all instruments */
        for (int i = 0; i < app->instrument_count; i++) {
            Instrument* inst = app->instruments[i];
            if (inst && inst->on_update) {
                inst->on_update(inst, app->flight_data, app->delta_time);
            }
        }
        /* 5. Render */
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
        SDL_RenderClear(app->renderer);

        /* Draw background image */
        if (app->bg_texture) {
            float bg_w = 8026.0f;
            float bg_h = 3136.0f;
            float scale_x = (float)app->screen_w / bg_w;
            float scale_y = (float)app->screen_h / bg_h;
            float scale = (scale_x < scale_y) ? scale_x : scale_y;
            int render_w = (int)(bg_w * scale);
            int render_h = (int)(bg_h * scale);
            int offset_x = (app->screen_w - render_w) / 2;
            int offset_y = (app->screen_h - render_h) / 2;
            SDL_Rect bg_rect = {offset_x, offset_y, render_w, render_h};
            SDL_RenderCopy(app->renderer, app->bg_texture, NULL, &bg_rect);

            /* ---- Master Warning / Caution Lights ---- */
            Uint32 ticks = SDL_GetTicks();
            int blink_warn = (snapshot.master_warning && (ticks % 600 < 300));
            int blink_caut = (snapshot.master_caution && (ticks % 600 < 300));

            if (blink_warn || blink_caut) {
                SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);

                // Captain Side
                if (blink_warn) {
                    SDL_Rect r_cw = {
                        bg_rect.x + (int)(bg_rect.w * 0.197f),
                        bg_rect.y + (int)(bg_rect.h * 0.088f),
                        (int)(bg_rect.w * (0.222f - 0.197f)),
                        (int)(bg_rect.h * (0.125f - 0.088f))
                    };
                    SDL_SetRenderDrawColor(app->renderer, 255, 0, 0, 180);
                    SDL_RenderFillRect(app->renderer, &r_cw);
                }
                if (blink_caut) {
                    SDL_Rect r_cc = {
                        bg_rect.x + (int)(bg_rect.w * 0.223f),
                        bg_rect.y + (int)(bg_rect.h * 0.088f),
                        (int)(bg_rect.w * (0.248f - 0.223f)),
                        (int)(bg_rect.h * (0.125f - 0.088f))
                    };
                    SDL_SetRenderDrawColor(app->renderer, 255, 180, 0, 180);
                    SDL_RenderFillRect(app->renderer, &r_cc);
                }

                // FO Side
                if (blink_caut) {
                    SDL_Rect r_fc = {
                        bg_rect.x + (int)(bg_rect.w * 0.768f),
                        bg_rect.y + (int)(bg_rect.h * 0.085f),
                        (int)(bg_rect.w * (0.793f - 0.768f)),
                        (int)(bg_rect.h * (0.123f - 0.085f))
                    };
                    SDL_SetRenderDrawColor(app->renderer, 255, 180, 0, 180);
                    SDL_RenderFillRect(app->renderer, &r_fc);
                }
                if (blink_warn) {
                    SDL_Rect r_fw = {
                        bg_rect.x + (int)(bg_rect.w * 0.794f),
                        bg_rect.y + (int)(bg_rect.h * 0.085f),
                        (int)(bg_rect.w * (0.819f - 0.794f)),
                        (int)(bg_rect.h * (0.123f - 0.085f))
                    };
                    SDL_SetRenderDrawColor(app->renderer, 255, 0, 0, 180);
                    SDL_RenderFillRect(app->renderer, &r_fw);
                }
            }
        }

        /* Draw instruments that are NOT zoomed first */
        for (int i = 0; i < app->instrument_count; i++) {
            if (i == app->zoomed_instrument_index) continue;
            Instrument* inst = app->instruments[i];
            if (inst && inst->on_render && app->instrument_targets[i]) {
                SDL_Rect phys = app->instrument_base_rects[i];

                /* Render instrument to its target texture at native 772x721 */
                SDL_SetRenderTarget(app->renderer, app->instrument_targets[i]);
                SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
                SDL_RenderClear(app->renderer);
                inst->on_render(inst, app->renderer);
                inst->needs_redraw = 0;

                /* Restore default render target and copy scaled texture */
                SDL_SetRenderTarget(app->renderer, NULL);
                SDL_RenderCopy(app->renderer, app->instrument_targets[i], NULL, &phys);
            }
        }

        /* Draw zoomed instrument on top */
        if (app->zoomed_instrument_index >= 0 && app->zoomed_instrument_index < app->instrument_count) {
            Instrument* inst = app->instruments[app->zoomed_instrument_index];
            if (inst && inst->on_render && app->instrument_targets[app->zoomed_instrument_index]) {
                /* Draw a dark overlay to dim the background */
                SDL_RenderSetClipRect(app->renderer, NULL);
                SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 150);
                SDL_Rect full_rect = {0, 0, app->screen_w, app->screen_h};
                SDL_RenderFillRect(app->renderer, &full_rect);

                int native_w = 772;
                int native_h = 721;
                if (inst->name && strstr(inst->name, "FMC")) {
                    native_w = 643;
                    native_h = 992;
                }

                SDL_Rect phys;
                phys.w = native_w;
                phys.h = native_h;
                phys.x = (app->screen_w - native_w) / 2;
                phys.y = (app->screen_h - native_h) / 2;

                /* Render instrument to its target texture at native resolution */
                SDL_SetRenderTarget(app->renderer, app->instrument_targets[app->zoomed_instrument_index]);
                SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
                SDL_RenderClear(app->renderer);
                inst->on_render(inst, app->renderer);
                inst->needs_redraw = 0;

                /* Restore default render target and copy texture */
                SDL_SetRenderTarget(app->renderer, NULL);
                SDL_RenderCopy(app->renderer, app->instrument_targets[app->zoomed_instrument_index], NULL, &phys);
            }
        }

        SDL_RenderSetClipRect(app->renderer, NULL);

        /* Draw zoom buttons on each instrument */
        for (int i = 0; i < app->instrument_count; i++) {
            if (app->zoomed_instrument_index >= 0 && app->zoomed_instrument_index != i) {
                continue; /* If zoomed in, only draw the button for the zoomed instrument */
            }

            Instrument* inst = app->instruments[i];
            if (!inst) continue;

            SDL_Rect phys;
            if (app->zoomed_instrument_index == i) {
                int native_w = 772;
                int native_h = 721;
                if (inst->name && strstr(inst->name, "FMC")) {
                    native_w = 643;
                    native_h = 992;
                }
                phys.w = native_w;
                phys.h = native_h;
                phys.x = (app->screen_w - native_w) / 2;
                phys.y = (app->screen_h - native_h) / 2;
            } else {
                phys = app->instrument_base_rects[i];
            }

            SDL_Texture* btn_tex = (app->zoomed_instrument_index == i) ? app->btn_sub : app->btn_full_screen;
            if (btn_tex) {
                int btn_size = (app->zoomed_instrument_index == i) ? 32 : (phys.w * 32 / 772);
                if (btn_size < 16) btn_size = 16;
                SDL_Rect btn_rect = {
                    phys.x + phys.w - btn_size - 4,
                    phys.y + 4,
                    btn_size,
                    btn_size
                };
                SDL_RenderCopy(app->renderer, btn_tex, NULL, &btn_rect);
            }
        }

        SDL_RenderPresent(app->renderer);

        /* 6. Frame rate cap */
        if (app->target_frame_time > 0.0f) {
            Uint32 elapsed = SDL_GetTicks() - frame_start;
            float remaining = (app->target_frame_time * 1000.0f) - (float)elapsed;
            if (remaining > 1.0f) {
                SDL_Delay((Uint32)remaining);
            }
        }
    }
}

/* =========================================================================
 *  Quit event handler (registered with event system)
 * ========================================================================= */

static int on_quit_event(const SDL_Event* event, void* userdata)
{
    (void)event;
    App* app = (App*)userdata;
    if (event->type == SDL_QUIT) {
        app->running = 0;
        return 1;  /* Consumed */
    }
    if (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_ESCAPE) {
        app->running = 0;
        return 1;
    }
    return 0;
}

/**
 * @brief Bridge: route unconsumed events to all instruments.
 *        Registered with SDL_FIRSTEVENT (all types), runs after quit handler.
 */
static int instrument_event_bridge(const SDL_Event* event, void* userdata)
{

    App* app = (App*)userdata;

    if (event->type == SDL_MOUSEBUTTONDOWN) {
        int mx = event->button.x;
        int my = event->button.y;
        /* Check zoom buttons first */
        for (int i = 0; i < app->instrument_count; i++) {
            Instrument* inst = app->instruments[i];
            if (!inst) continue;

            SDL_Rect phys;
            if (app->zoomed_instrument_index == i) {
                int native_w = 772;
                int native_h = 721;
                if (inst->name && strstr(inst->name, "FMC")) {
                    native_w = 643;
                    native_h = 992;
                }
                phys.w = native_w;
                phys.h = native_h;
                phys.x = (app->screen_w - native_w) / 2;
                phys.y = (app->screen_h - native_h) / 2;
            } else {
                phys = app->instrument_base_rects[i];
            }

            int btn_size = (app->zoomed_instrument_index == i) ? 32 : (phys.w * 32 / 772);
            if (btn_size < 16) btn_size = 16;
            
            SDL_Rect btn_rect = {
                phys.x + phys.w - btn_size - 4,
                phys.y + 4,
                btn_size,
                btn_size
            };

            if (mx >= btn_rect.x && mx <= btn_rect.x + btn_rect.w &&
                my >= btn_rect.y && my <= btn_rect.y + btn_rect.h) {
                if (app->zoomed_instrument_index == i) {
                    app->zoomed_instrument_index = -1; /* zoom out */
                } else {
                    app->zoomed_instrument_index = i; /* zoom in */
                }
                layout_instruments(app); /* reapply layout */
                return 1; /* consumed */
            }
        }
    }

    /* Route to zoomed instrument first */
    if (app->zoomed_instrument_index >= 0 && app->zoomed_instrument_index < app->instrument_count) {
        Instrument* inst = app->instruments[app->zoomed_instrument_index];
        if (inst && inst->on_event) {
            SDL_Event local_event = *event;
            if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP || event->type == SDL_MOUSEMOTION) {
                int native_w = 772;
                int native_h = 721;
                if (inst->name && strstr(inst->name, "FMC")) {
                    native_w = 643;
                    native_h = 992;
                }

                SDL_Rect phys;
                phys.w = native_w;
                phys.h = native_h;
                phys.x = (app->screen_w - native_w) / 2;
                phys.y = (app->screen_h - native_h) / 2;
                
                float scale_x = (float)native_w / (float)phys.w;
                float scale_y = (float)native_h / (float)phys.h;
                
                if (event->type == SDL_MOUSEMOTION) {
                    local_event.motion.x = (Sint32)((event->motion.x - phys.x) * scale_x);
                    local_event.motion.y = (Sint32)((event->motion.y - phys.y) * scale_y);
                } else {
                    local_event.button.x = (Sint32)((event->button.x - phys.x) * scale_x);
                    local_event.button.y = (Sint32)((event->button.y - phys.y) * scale_y);
                }
            }
            if (inst->on_event(inst, &local_event)) {
                return 1;
            }
        }
    } else {
        /* Route to all instruments */
        for (int i = 0; i < app->instrument_count; i++) {
            Instrument* inst = app->instruments[i];
            if (inst && inst->on_event) {
                SDL_Event local_event = *event;
                if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP || event->type == SDL_MOUSEMOTION) {
                    SDL_Rect phys = app->instrument_base_rects[i];

                    int native_w = 772;
                    int native_h = 721;
                    if (inst->name && strstr(inst->name, "FMC")) {
                        native_w = 643;
                        native_h = 992;
                    }

                    float scale_x = (float)native_w / (float)phys.w;
                    float scale_y = (float)native_h / (float)phys.h;

                    if (event->type == SDL_MOUSEMOTION) {
                        local_event.motion.x = (Sint32)((event->motion.x - phys.x) * scale_x);
                        local_event.motion.y = (Sint32)((event->motion.y - phys.y) * scale_y);
                    } else {
                        local_event.button.x = (Sint32)((event->button.x - phys.x) * scale_x);
                        local_event.button.y = (Sint32)((event->button.y - phys.y) * scale_y);
                    }
                }
                int consumed = inst->on_event(inst, &local_event);
                if (consumed) {
                    return 1;  /* Consumed by this instrument */
                }
            }
        }
    }
    return 0;  /* Not consumed — allow other handlers */
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

int app_run_with_config(const char* config_path)
{
    int ret = -1;
    App  app_state;
    App* app = &app_state;

    /* Zero-initialize everything for safe cleanup */
    memset(app, 0, sizeof(App));

    /* 1. Load config */
    app->config = config_load(config_path);
    if (!app->config) {
        fprintf(stderr, "FATAL: Cannot load config (no defaults?)\n");
        return -1;
    }

    /* 2. Init logging */
    {
        int log_level = (int)config_get_int(app->config, "logging", "log_level", LOG_LVL_INFO);
        const char* log_file = config_get_str(app->config, "logging", "log_file", "");
        if (logger_init((LogLevel)log_level, log_file) != 0) {
            fprintf(stderr, "WARN: Logger init failed, using stderr fallback\n");
        }
    }

    fprintf(stderr, "[STARTUP] 1/7 Config loaded\n");
    LOG_INFO("=== Flight Cockpit Simulation System ===");
    LOG_INFO("Config: %s", config_path);

    /* 3. Init SDL */
    fprintf(stderr, "[STARTUP] 2/7 Initializing SDL...\n");
    if (init_sdl(app) != 0) {
        LOG_ERROR("SDL initialization failed");
        goto cleanup;
    }
    fprintf(stderr, "[STARTUP] 2/7 SDL OK (window %dx%d)\n", app->screen_w, app->screen_h);

    /* 4. Create event system */
    fprintf(stderr, "[STARTUP] 3/7 Creating event system...\n");
    app->events = eventsys_create();
    if (!app->events) {
        LOG_ERROR("Event system creation failed");
        goto cleanup;
    }
    eventsys_register(app->events, on_quit_event, app, SDL_QUIT);
    eventsys_register(app->events, on_quit_event, app, SDL_KEYDOWN);

    /* 5. Create shared flight data */
    fprintf(stderr, "[STARTUP] 4/7 Creating flight data...\n");
    app->flight_data = flight_data_create();
    if (!app->flight_data) {
        LOG_ERROR("Flight data creation failed");
        goto cleanup;
    }

    /* 6. Create FMC shared state */
    fprintf(stderr, "[STARTUP] 5/7 Creating FMC state...\n");
    app->fmc_state = fmc_state_create();
    if (!app->fmc_state) {
        LOG_WARN("FMC state creation failed — FMC disabled");
    } else {
        nav_database_init(app->fmc_state);
        fprintf(stderr, "[STARTUP] Nav DB: %d airports, %d waypoints\n",
                app->fmc_state->nav_apt_count, app->fmc_state->nav_wpt_count);

        /* Load spatial hash from earth_fix.dat / earth_nav.dat */
        nav_database_load_files(app->fmc_state);
        fprintf(stderr, "[STARTUP] Spatial hash: %d entries loaded\n",
                app->fmc_state->spatial_hash ?
                app->fmc_state->spatial_hash->total_entries : 0);
    }

    /* 7. Create instruments */
    fprintf(stderr, "[STARTUP] 6/7 Creating instruments...\n");
    if (create_instruments(app) <= 0) {
        LOG_WARN("No instruments created — running with empty window");
    }
    layout_instruments(app);
    init_instruments(app);

    /* Register instrument event bridge — routes events to instruments.
     * Registered AFTER quit handler so ESC/QUIT still work first. */
    eventsys_register(app->events, instrument_event_bridge, app, SDL_FIRSTEVENT);

    fprintf(stderr, "[STARTUP] 6/7 %d instruments ready\n", app->instrument_count);

    /* 8. Start background threads */
    fprintf(stderr, "[STARTUP] 7/7 Starting data source & alerts...\n");

    /* 8a. GPWS Audio alert system */
    {
        int alerts_enabled = config_get_bool(app->config, "audio", "alerts_enabled", 1);
        app->alert_sys = alert_system_create();
        if (app->alert_sys) {
            alert_system_set_enabled(app->alert_sys, alerts_enabled);
            if (alert_system_audio_ok(app->alert_sys)) {
                fprintf(stderr, "[STARTUP] GPWS alert system: OK\n");
            } else {
                fprintf(stderr, "[STARTUP] GPWS alert system: audio unavailable, muted\n");
            }
        }
    }

    /* 8. Main loop - must be set BEFORE starting threads so they don't exit immediately */
    app->running = 1;

    /* 8b. Data source (mock or UDP) */
    start_data_source(app);
    fprintf(stderr, "[STARTUP] 7/7 Entering main loop\n");

    /* 8c. Start Cabin Map Display */
    app->map_display = map_display_create(app->config, app->fmc_state);

    /* 9. Main loop */
    main_loop(app);

    ret = 0;  /* Clean exit */

cleanup:
    /* 10. Shutdown — reverse order */
    LOG_INFO("Shutting down...");

    /* Stop UDP thread first */
    if (app->udp_thread) {
        thread_stop(app->udp_thread, 2000);
        thread_free(app->udp_thread);
        app->udp_thread = NULL;
    }

    /* Close X-Plane send socket */
    if (app->xp_send_sock) {
        udp_socket_destroy(app->xp_send_sock);
        app->xp_send_sock = NULL;
    }

    /* Stop mock data thread */
    if (app->mock_thread) {
        if (app->mock_ctx) app->mock_ctx->running = 0;
        thread_stop(app->mock_thread, 2000);
        thread_free(app->mock_thread);
        app->mock_thread = NULL;
    }
    if (app->mock_ctx) {
        mock_data_free(app->mock_ctx);
        app->mock_ctx = NULL;
    }

    /* Stop Cabin Map Display */
    if (app->map_display) {
        map_display_destroy(app->map_display);
        app->map_display = NULL;
    }

    /* Destroy alert system */
    if (app->alert_sys) {
        alert_system_destroy(app->alert_sys);
        app->alert_sys = NULL;
    }

    /* Destroy instruments */
    destroy_instruments(app);

    /* Free FMC state */
    if (app->fmc_state) {
        fmc_state_free(app->fmc_state);
        app->fmc_state = NULL;
    }

    /* Free flight data */
    if (app->flight_data) {
        flight_data_destroy(app->flight_data);
        app->flight_data = NULL;
    }

    /* Free event system */
    if (app->events) {
        eventsys_destroy(app->events);
        app->events = NULL;
    }

    /* SDL cleanup */
    if (app->renderer) {
        SDL_DestroyRenderer(app->renderer);
        app->renderer = NULL;
    }
    if (app->window) {
        SDL_DestroyWindow(app->window);
        app->window = NULL;
    }
    font_system_shutdown();
    TTF_Quit();
    SDL_Quit();

    /* Free config */
    if (app->config) {
        config_free(app->config);
        app->config = NULL;
    }

    logger_shutdown();

    if (ret == 0) {
        printf("=== Flight Cockpit Simulation System exited cleanly ===\n");
    } else {
        fprintf(stderr, "=== Flight Cockpit Simulation System exited with errors ===\n");
    }

    return ret;
}
