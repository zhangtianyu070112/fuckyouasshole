/**
 * @file    cabin_old.c
 * @brief   Native SDL2 cabin map display — separate SDL window (tile-based).
 *
 * Creates an independent always-on-top window for the cabin moving map.
 * Fetches 高德 map tiles via HTTP in a background thread.
 *
 * This is the original tile-based cabin extracted from commit c8b359c as a
 * standalone module. It coexists with the current OpenGL 3D globe MapDisplay.
 */

#include "cabin_old.h"
#include "geo_projection.h"
#include "trajectory_render.h"
#include "http.h"
#include "config.h"
#include "utils/logger.h"
#include "utils/font_manager.h"

#include <SDL2/SDL_image.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 *  Constants
 * ========================================================================= */

#define AMAP_TILE_HOST      "webrd01.is.autonavi.com"
#define AMAP_TILE_PORT      80
#define AMAP_TILE_PATH_FMT  "/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x=%d&y=%d&z=%d"

#define CABIN_WIN_W         1024
#define CABIN_WIN_H         680
#define CABIN_ZOOM_DEFAULT  8
#define CABIN_ZOOM_MIN      3
#define CABIN_ZOOM_MAX      17
#define CABIN_FETCH_MS      5000
#define CABIN_POS_EASE      0.35f
#define CABIN_HEADER_H      32
#define CABIN_DATABAR_H     60
#define CABIN_PROGRESS_H    3

/* =========================================================================
 *  Helpers
 * ========================================================================= */

static void make_tile_path(int tx, int ty, int zoom, char* buf, size_t bufsz)
{
    snprintf(buf, bufsz, AMAP_TILE_PATH_FMT, tx, ty, zoom);
}

static void make_tile_key(int tx, int ty, int zoom, char* buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%d/%d/%d", zoom, tx, ty);
}

/* =========================================================================
 *  Tile fetch (background thread)
 * ========================================================================= */

static SDL_Surface* fetch_tile_surface(int tx, int ty, int zoom)
{
    char path[256];
    make_tile_path(tx, ty, zoom, path, sizeof(path));

    HTTPResponse* resp = http_get(AMAP_TILE_HOST, AMAP_TILE_PORT, path);
    if (!resp || resp->status_code != 200) {
        if (resp) http_response_free(resp);
        return NULL;
    }
    if (!resp->body || resp->body_len == 0) {
        http_response_free(resp);
        return NULL;
    }

    SDL_RWops* rw = SDL_RWFromConstMem(resp->body, (int)resp->body_len);
    if (!rw) { http_response_free(resp); return NULL; }

    SDL_Surface* surf = IMG_Load_RW(rw, 0);
    SDL_RWclose(rw);
    http_response_free(resp);
    return surf;
}

static int tile_fetch_thread_func(void* data)
{
    CabinOld* co = (CabinOld*)data;

    while (SDL_AtomicGet(&co->fetch_running)) {
        if (!SDL_AtomicGet(&co->fetch_pending)) {
            SDL_Delay(250);
            continue;
        }

        SDL_LockMutex(co->fetch_mutex);
        double clat = co->fetch_lat, clon = co->fetch_lon;
        int zoom = co->fetch_zoom;
        SDL_AtomicSet(&co->fetch_pending, 0);
        SDL_UnlockMutex(co->fetch_mutex);

        if (zoom < CABIN_ZOOM_MIN || zoom > CABIN_ZOOM_MAX) continue;
        if (clat == 0.0 && clon == 0.0) continue;

        double ctx, cty;
        geo_to_tile(clat, clon, zoom, &ctx, &cty);
        int cx = (int)floor(ctx), cy = (int)floor(cty);
        int max_tile = (1 << zoom) - 1;

        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int tx = cx + dx, ty = cy + dy;
                if (tx < 0 || tx > max_tile || ty < 0 || ty > max_tile) continue;

                SDL_Surface* surf = fetch_tile_surface(tx, ty, zoom);
                if (!surf) continue;

                char key[32];
                make_tile_key(tx, ty, zoom, key, sizeof(key));

                SDL_LockMutex(co->pending_mutex);
                if (co->pending_count < CABIN_PENDING_MAX) {
                    co->pending[co->pending_count].surf = surf;
                    strncpy(co->pending[co->pending_count].key, key, 31);
                    co->pending[co->pending_count].key[31] = '\0';
                    co->pending_count++;
                } else {
                    SDL_FreeSurface(surf);
                }
                SDL_UnlockMutex(co->pending_mutex);
            }
        }
    }
    return 0;
}

/* =========================================================================
 *  Create / Destroy
 * ========================================================================= */

CabinOld* cabin_old_create(const Config* cfg, FMCState* fmc)
{
    if (!cfg) return NULL;

    const char* api_key = config_get_str(cfg, "map", "amap_api_key", "");
    int zoom     = (int)config_get_int(cfg, "map", "map_zoom", CABIN_ZOOM_DEFAULT);
    int interval = (int)config_get_int(cfg, "map", "map_update_ms", CABIN_FETCH_MS);
    if (zoom < CABIN_ZOOM_MIN) zoom = CABIN_ZOOM_MIN;
    if (zoom > CABIN_ZOOM_MAX) zoom = CABIN_ZOOM_MAX;

    CabinOld* co = (CabinOld*)calloc(1, sizeof(CabinOld));
    if (!co) return NULL;

    strncpy(co->api_key, api_key, sizeof(co->api_key) - 1);
    co->zoom = zoom;
    co->tile_size = GEO_TILE_SIZE;
    co->fetch_interval_ms = interval;
    co->fmc = fmc;

    /* Create always-on-top window so it's visible above fullscreen cockpit */
    co->window = SDL_CreateWindow("Cabin Moving Map",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CABIN_WIN_W, CABIN_WIN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALWAYS_ON_TOP);
    if (!co->window) {
        LOG_ERROR("CabinOld: SDL_CreateWindow failed: %s", SDL_GetError());
        free(co); return NULL;
    }

    co->renderer = SDL_CreateRenderer(co->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!co->renderer) {
        LOG_ERROR("CabinOld: SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(co->window); free(co); return NULL;
    }

    SDL_GetWindowSize(co->window, &co->win_w, &co->win_h);
    co->map_rect.x = 0;
    co->map_rect.y = CABIN_HEADER_H;
    co->map_rect.w = co->win_w;
    co->map_rect.h = co->win_h - CABIN_HEADER_H - CABIN_DATABAR_H - CABIN_PROGRESS_H;
    if (co->map_rect.h < 40) co->map_rect.h = 40;

    /* Tile cache uses the cabin renderer */
    co->tile_cache = tile_cache_create(co->renderer);
    co->data_mutex  = SDL_CreateMutex();
    co->fetch_mutex = SDL_CreateMutex();
    co->pending_mutex = SDL_CreateMutex();

    if (!co->tile_cache || !co->data_mutex || !co->fetch_mutex || !co->pending_mutex) {
        LOG_ERROR("CabinOld: failed to create subsystems");
        cabin_old_destroy(co); return NULL;
    }

    SDL_AtomicSet(&co->fetch_running, 1);
    co->fetch_thread = SDL_CreateThread(tile_fetch_thread_func, "MapTileFetch", co);
    if (!co->fetch_thread) {
        LOG_ERROR("CabinOld: SDL_CreateThread failed");
        cabin_old_destroy(co); return NULL;
    }

    co->zoom_start_ms = SDL_GetTicks64();
    co->base_zoom = (double)zoom;

    LOG_INFO("CabinOld: created window %dx%d zoom=%d fetch=%dms",
             co->win_w, co->win_h, zoom, interval);
    return co;
}

void cabin_old_destroy(CabinOld* co)
{
    if (!co) return;
    SDL_AtomicSet(&co->fetch_running, 0);
    if (co->fetch_thread) SDL_WaitThread(co->fetch_thread, NULL);

    tile_cache_destroy(co->tile_cache);
    if (co->data_mutex)    SDL_DestroyMutex(co->data_mutex);
    if (co->fetch_mutex)   SDL_DestroyMutex(co->fetch_mutex);
    if (co->pending_mutex) SDL_DestroyMutex(co->pending_mutex);
    for (int i = 0; i < co->pending_count; i++)
        if (co->pending[i].surf) SDL_FreeSurface(co->pending[i].surf);

    if (co->renderer) SDL_DestroyRenderer(co->renderer);
    if (co->window)   SDL_DestroyWindow(co->window);
    free(co);
    LOG_INFO("CabinOld: destroyed");
}

/* =========================================================================
 *  Update
 * ========================================================================= */

void cabin_old_update_position(CabinOld* co, const FlightDataValues* fd)
{
    if (!co || !fd) return;
    SDL_LockMutex(co->data_mutex);
    co->last_fd = *fd;
    SDL_UnlockMutex(co->data_mutex);
}

/* =========================================================================
 *  Internal helpers
 * ========================================================================= */

static void calc_progress(const FlightDataValues* fd, const FMCState* fmc,
                          double* flown_nm, double* total_nm,
                          double* remain_nm, double* progress_pct)
{
    *flown_nm = *total_nm = *remain_nm = *progress_pct = 0.0;
    if (!fmc || fmc->flight_plan.waypoint_count < 2) return;
    const FlightPlan* fp = &fmc->flight_plan;
    int n = fp->waypoint_count;

    for (int i = 0; i < n - 1; i++)
        *total_nm += geo_haversine_nm(
            fp->waypoints[i].pos.lat_deg, fp->waypoints[i].pos.lon_deg,
            fp->waypoints[i+1].pos.lat_deg, fp->waypoints[i+1].pos.lon_deg);
    if (*total_nm <= 0.0) return;

    int active = fp->active_waypoint_index;
    if (active <= 0) active = 0;
    if (active >= n) active = n - 1;
    for (int i = 0; i < active; i++)
        *flown_nm += geo_haversine_nm(
            fp->waypoints[i].pos.lat_deg, fp->waypoints[i].pos.lon_deg,
            fp->waypoints[i+1].pos.lat_deg, fp->waypoints[i+1].pos.lon_deg);
    if (fd->lat_deg != 0.0 && active < n)
        *flown_nm += geo_haversine_nm(
            fp->waypoints[active].pos.lat_deg, fp->waypoints[active].pos.lon_deg,
            fd->lat_deg, fd->lon_deg);

    *remain_nm = geo_haversine_nm(fd->lat_deg, fd->lon_deg,
        fp->waypoints[n-1].pos.lat_deg, fp->waypoints[n-1].pos.lon_deg);
    *progress_pct = (*total_nm > 0.0) ? (*flown_nm / *total_nm * 100.0) : 0.0;
    if (*progress_pct < 0.0) *progress_pct = 0.0;
    if (*progress_pct > 100.0) *progress_pct = 100.0;
}

static void interpolate(CabinOld* co)
{
    FlightDataValues fd;
    SDL_LockMutex(co->data_mutex); fd = co->last_fd; SDL_UnlockMutex(co->data_mutex);
    if (fd.lat_deg == 0.0 && fd.lon_deg == 0.0) return;
    if (co->disp_lat == 0.0 && co->disp_lon == 0.0) {
        co->disp_lat = fd.lat_deg; co->disp_lon = fd.lon_deg; return;
    }
    co->disp_lat += (fd.lat_deg - co->disp_lat) * CABIN_POS_EASE;
    co->disp_lon += (fd.lon_deg - co->disp_lon) * CABIN_POS_EASE;
}

static void drain_pending(CabinOld* co)
{
    SDL_LockMutex(co->pending_mutex);
    for (int i = 0; i < co->pending_count; i++) {
        if (co->pending[i].surf) {
            tile_cache_put(co->tile_cache, co->pending[i].key, co->pending[i].surf);
            co->pending[i].surf = NULL;
        }
    }
    co->pending_count = 0;
    SDL_UnlockMutex(co->pending_mutex);
}

static void trigger_fetch(CabinOld* co)
{
    FlightDataValues fd;
    SDL_LockMutex(co->data_mutex); fd = co->last_fd; SDL_UnlockMutex(co->data_mutex);
    if (fd.lat_deg == 0.0 && fd.lon_deg == 0.0) return;
    if (!SDL_AtomicGet(&co->fetch_pending)) {
        SDL_LockMutex(co->fetch_mutex);
        co->fetch_lat = co->disp_lat; co->fetch_lon = co->disp_lon;
        co->fetch_zoom = co->zoom;
        SDL_AtomicSet(&co->fetch_pending, 1);
        SDL_UnlockMutex(co->fetch_mutex);
    }
}

static void update_auto_zoom(CabinOld* co)
{
    if (co->base_zoom <= 0.0) return;
    double elapsed = (double)(SDL_GetTicks64() - co->zoom_start_ms) / 1000.0;
    double phase = fmod(elapsed, 60.0) / 60.0;
    int z = (int)(co->base_zoom + sin(phase * M_PI * 2.0) * 2.5 + 0.5);
    if (z < CABIN_ZOOM_MIN) z = CABIN_ZOOM_MIN;
    if (z > CABIN_ZOOM_MAX) z = CABIN_ZOOM_MAX;
    co->zoom = z;
}

/* =========================================================================
 *  Render functions
 * ========================================================================= */

static void render_tiles(CabinOld* co)
{
    SDL_Renderer* r = co->renderer;
    SDL_Rect* mr = &co->map_rect;

    SDL_RenderSetClipRect(r, mr);
    SDL_SetRenderDrawColor(r, 13, 26, 43, 255);
    SDL_RenderFillRect(r, mr);

    double clat = co->disp_lat, clon = co->disp_lon;
    int zoom = co->zoom;
    if (clat == 0.0 && clon == 0.0) { SDL_RenderSetClipRect(r, NULL); return; }

    double ctx, cty;
    geo_to_tile(clat, clon, zoom, &ctx, &cty);
    int cxi = (int)floor(ctx), cyi = (int)floor(cty);
    int cpx = (int)((ctx - (double)cxi) * (double)GEO_TILE_SIZE);
    int cpy = (int)((cty - (double)cyi) * (double)GEO_TILE_SIZE);
    int scx = mr->x + mr->w / 2, scy = mr->y + mr->h / 2;
    int max_tile = (1 << zoom) - 1;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int tx = cxi + dx, ty = cyi + dy;
            if (tx < 0 || tx > max_tile || ty < 0 || ty > max_tile) continue;
            char key[32];
            make_tile_key(tx, ty, zoom, key, sizeof(key));
            SDL_Texture* tex = tile_cache_get(co->tile_cache, key);
            if (!tex) continue;
            int sx = tx * GEO_TILE_SIZE - (cxi * GEO_TILE_SIZE + cpx) + scx;
            int sy = ty * GEO_TILE_SIZE - (cyi * GEO_TILE_SIZE + cpy) + scy;
            SDL_Rect dst = { sx, sy, GEO_TILE_SIZE, GEO_TILE_SIZE };
            SDL_RenderCopy(r, tex, NULL, &dst);
        }
    }
    SDL_RenderSetClipRect(r, NULL);
}

static void render_header(CabinOld* co)
{
    SDL_Renderer* r = co->renderer;
    FlightDataValues fd;
    SDL_LockMutex(co->data_mutex); fd = co->last_fd; SDL_UnlockMutex(co->data_mutex);

    SDL_Rect hdr = { 0, 0, co->win_w, CABIN_HEADER_H };
    SDL_SetRenderDrawColor(r, 10, 30, 50, 255);
    SDL_RenderFillRect(r, &hdr);
    SDL_SetRenderDrawColor(r, 42, 106, 172, 255);
    SDL_Rect line = { 0, CABIN_HEADER_H - 2, co->win_w, 2 };
    SDL_RenderFillRect(r, &line);

    char buf[64] = "----  →  ----";
    if (co->fmc && co->fmc->flight_plan.waypoint_count >= 2)
        snprintf(buf, sizeof(buf), "%s  →  %s",
                 co->fmc->flight_plan.departure.icao,
                 co->fmc->flight_plan.arrival.icao);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    font_draw(r, co->win_w / 2, CABIN_HEADER_H / 2, buf, 12, FONT_BOLD);
}

static void render_data_bar(CabinOld* co)
{
    SDL_Renderer* r = co->renderer;
    int bar_y = co->win_h - CABIN_DATABAR_H;
    FlightDataValues fd;
    SDL_LockMutex(co->data_mutex); fd = co->last_fd; SDL_UnlockMutex(co->data_mutex);

    double flown, total, remain, pct;
    calc_progress(&fd, co->fmc, &flown, &total, &remain, &pct);

    SDL_Rect bar = { 0, bar_y, co->win_w, CABIN_DATABAR_H };
    SDL_SetRenderDrawColor(r, 10, 30, 50, 240);
    SDL_RenderFillRect(r, &bar);
    SDL_SetRenderDrawColor(r, 42, 106, 172, 255);
    SDL_Rect top = { 0, bar_y, co->win_w, 2 };
    SDL_RenderFillRect(r, &top);

    int col_w = co->win_w / 7;
    const char* labels[] = {"GS","ALT","HDG","TAS","OAT","DIST","ETA"};
    char vals[7][32];
    snprintf(vals[0], 32, "%d KTS", (int)fd.gs_kts);
    snprintf(vals[1], 32, "%d FT",  (int)fd.alt_msl_ft);
    snprintf(vals[2], 32, "%d°",   (int)fd.heading_true_deg);
    snprintf(vals[3], 32, "%d KTS", (int)fd.tas_kts);
    if (fd.oat_c > -99.0f) snprintf(vals[4], 32, "%.1f°C", (double)fd.oat_c);
    else snprintf(vals[4], 32, "--°C");
    if (remain > 1.0) snprintf(vals[5], 32, "%d NM", (int)remain);
    else snprintf(vals[5], 32, "--- NM");
    if (fd.gs_kts > 30.0f && remain > 1.0) {
        double h = remain / (double)fd.gs_kts;
        snprintf(vals[6], 32, "%02d:%02d", (int)h, (int)((h-(int)h)*60.0));
    } else snprintf(vals[6], 32, "--:--");

    for (int i = 0; i < 7; i++) {
        int cx = col_w * i + col_w / 2;
        SDL_SetRenderDrawColor(r, 138, 180, 216, 255);
        font_draw(r, cx, bar_y + 10, labels[i], 8, FONT_REGULAR);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        font_draw(r, cx, bar_y + 34, vals[i], 14, FONT_BOLD);
    }
}

static void render_progress(CabinOld* co)
{
    SDL_Renderer* r = co->renderer;
    int prog_y = co->win_h - CABIN_DATABAR_H - CABIN_PROGRESS_H;
    FlightDataValues fd;
    SDL_LockMutex(co->data_mutex); fd = co->last_fd; SDL_UnlockMutex(co->data_mutex);

    double flown, total, remain, pct;
    calc_progress(&fd, co->fmc, &flown, &total, &remain, &pct);

    SDL_Rect track = { 0, prog_y, co->win_w, CABIN_PROGRESS_H };
    SDL_SetRenderDrawColor(r, 255, 255, 255, 20);
    SDL_RenderFillRect(r, &track);

    int fw = (int)((double)co->win_w * pct / 100.0);
    if (fw < 0) fw = 0; if (fw > co->win_w) fw = co->win_w;
    SDL_Rect fill = { 0, prog_y, fw, CABIN_PROGRESS_H };
    SDL_SetRenderDrawColor(r, 26, 115, 232, 255);
    SDL_RenderFillRect(r, &fill);
}

/* =========================================================================
 *  Public render entry
 * ========================================================================= */

void cabin_old_render(CabinOld* co)
{
    if (!co || !co->renderer) return;

    /* Update window size each frame (avoids event-polling conflicts with main loop) */
    SDL_GetWindowSize(co->window, &co->win_w, &co->win_h);
    co->map_rect.w = co->win_w;
    co->map_rect.h = co->win_h - CABIN_HEADER_H - CABIN_DATABAR_H - CABIN_PROGRESS_H;
    if (co->map_rect.h < 40) co->map_rect.h = 40;

    drain_pending(co);
    interpolate(co);
    update_auto_zoom(co);

    /* Route change detection → reset base zoom */
    if (co->fmc && co->fmc->flight_plan.waypoint_count >= 2) {
        const FlightPlan* fp = &co->fmc->flight_plan;
        if (fp->waypoint_count != co->last_wpt_count ||
            strcmp(fp->departure.icao, co->last_dep_icao) != 0 ||
            strcmp(fp->arrival.icao, co->last_arr_icao) != 0) {
            co->last_wpt_count = fp->waypoint_count;
            strncpy(co->last_dep_icao, fp->departure.icao, 7);
            strncpy(co->last_arr_icao, fp->arrival.icao, 7);
            co->base_zoom = (double)co->zoom;
            co->zoom_start_ms = SDL_GetTicks64();
        }
    }

    trigger_fetch(co);

    /* Render to cabin window */
    SDL_SetRenderDrawColor(co->renderer, 13, 26, 43, 255);
    SDL_RenderClear(co->renderer);

    render_tiles(co);

    /* Trajectory overlay — use interpolated position for aircraft */
    FlightDataValues fd;
    SDL_LockMutex(co->data_mutex); fd = co->last_fd; SDL_UnlockMutex(co->data_mutex);
    fd.lat_deg = co->disp_lat;
    fd.lon_deg = co->disp_lon;
    trajectory_render(co->renderer, co->disp_lat, co->disp_lon, co->zoom,
                      co->map_rect.w, co->map_rect.h,
                      co->map_rect.x, co->map_rect.y,
                      &fd, co->fmc);

    render_header(co);
    render_progress(co);
    render_data_bar(co);

    SDL_RenderPresent(co->renderer);
}
