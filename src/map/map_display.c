/**
 * @file    map_display.c
 * @brief   Native SDL2 cabin map display — separate SDL window.
 *
 * Creates an independent always-on-top window for the cabin moving map.
 * Fetches 高德 map tiles via HTTP in a background thread.
 */

#include "map_display.h"
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
    MapDisplay* md = (MapDisplay*)data;

    while (SDL_AtomicGet(&md->fetch_running)) {
        if (!SDL_AtomicGet(&md->fetch_pending)) {
            SDL_Delay(250);
            continue;
        }

        SDL_LockMutex(md->fetch_mutex);
        double clat = md->fetch_lat, clon = md->fetch_lon;
        int zoom = md->fetch_zoom;
        SDL_AtomicSet(&md->fetch_pending, 0);
        SDL_UnlockMutex(md->fetch_mutex);

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

                SDL_LockMutex(md->pending_mutex);
                if (md->pending_count < CABIN_PENDING_MAX) {
                    md->pending[md->pending_count].surf = surf;
                    strncpy(md->pending[md->pending_count].key, key, 31);
                    md->pending[md->pending_count].key[31] = '\0';
                    md->pending_count++;
                } else {
                    SDL_FreeSurface(surf);
                }
                SDL_UnlockMutex(md->pending_mutex);
            }
        }
    }
    return 0;
}

/* =========================================================================
 *  Create / Destroy
 * ========================================================================= */

MapDisplay* map_display_create(const Config* cfg, FMCState* fmc)
{
    if (!cfg) return NULL;

    const char* api_key = config_get_str(cfg, "map", "amap_api_key", "");
    int zoom     = (int)config_get_int(cfg, "map", "map_zoom", CABIN_ZOOM_DEFAULT);
    int interval = (int)config_get_int(cfg, "map", "map_update_ms", CABIN_FETCH_MS);
    if (zoom < CABIN_ZOOM_MIN) zoom = CABIN_ZOOM_MIN;
    if (zoom > CABIN_ZOOM_MAX) zoom = CABIN_ZOOM_MAX;

    MapDisplay* md = (MapDisplay*)calloc(1, sizeof(MapDisplay));
    if (!md) return NULL;

    strncpy(md->api_key, api_key, sizeof(md->api_key) - 1);
    md->zoom = zoom;
    md->tile_size = GEO_TILE_SIZE;
    md->fetch_interval_ms = interval;
    md->fmc = fmc;

    /* Create always-on-top window so it's visible above fullscreen cockpit */
    md->window = SDL_CreateWindow("Cabin Moving Map",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CABIN_WIN_W, CABIN_WIN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALWAYS_ON_TOP);
    if (!md->window) {
        LOG_ERROR("MapDisplay: SDL_CreateWindow failed: %s", SDL_GetError());
        free(md); return NULL;
    }

    md->renderer = SDL_CreateRenderer(md->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!md->renderer) {
        LOG_ERROR("MapDisplay: SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(md->window); free(md); return NULL;
    }

    SDL_GetWindowSize(md->window, &md->win_w, &md->win_h);
    md->map_rect.x = 0;
    md->map_rect.y = CABIN_HEADER_H;
    md->map_rect.w = md->win_w;
    md->map_rect.h = md->win_h - CABIN_HEADER_H - CABIN_DATABAR_H - CABIN_PROGRESS_H;
    if (md->map_rect.h < 40) md->map_rect.h = 40;

    /* Tile cache uses the cabin renderer */
    md->tile_cache = tile_cache_create(md->renderer);
    md->data_mutex  = SDL_CreateMutex();
    md->fetch_mutex = SDL_CreateMutex();
    md->pending_mutex = SDL_CreateMutex();

    if (!md->tile_cache || !md->data_mutex || !md->fetch_mutex || !md->pending_mutex) {
        LOG_ERROR("MapDisplay: failed to create subsystems");
        map_display_destroy(md); return NULL;
    }

    SDL_AtomicSet(&md->fetch_running, 1);
    md->fetch_thread = SDL_CreateThread(tile_fetch_thread_func, "MapTileFetch", md);
    if (!md->fetch_thread) {
        LOG_ERROR("MapDisplay: SDL_CreateThread failed");
        map_display_destroy(md); return NULL;
    }

    md->zoom_start_ms = SDL_GetTicks64();
    md->base_zoom = (double)zoom;

    LOG_INFO("MapDisplay: created window %dx%d zoom=%d fetch=%dms",
             md->win_w, md->win_h, zoom, interval);
    return md;
}

void map_display_destroy(MapDisplay* md)
{
    if (!md) return;
    SDL_AtomicSet(&md->fetch_running, 0);
    if (md->fetch_thread) SDL_WaitThread(md->fetch_thread, NULL);

    tile_cache_destroy(md->tile_cache);
    if (md->data_mutex)    SDL_DestroyMutex(md->data_mutex);
    if (md->fetch_mutex)   SDL_DestroyMutex(md->fetch_mutex);
    if (md->pending_mutex) SDL_DestroyMutex(md->pending_mutex);
    for (int i = 0; i < md->pending_count; i++)
        if (md->pending[i].surf) SDL_FreeSurface(md->pending[i].surf);

    if (md->renderer) SDL_DestroyRenderer(md->renderer);
    if (md->window)   SDL_DestroyWindow(md->window);
    free(md);
    LOG_INFO("MapDisplay: destroyed");
}

/* =========================================================================
 *  Update
 * ========================================================================= */

void map_display_update_position(MapDisplay* md, const FlightDataValues* fd)
{
    if (!md || !fd) return;
    SDL_LockMutex(md->data_mutex);
    md->last_fd = *fd;
    SDL_UnlockMutex(md->data_mutex);
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

static void interpolate(MapDisplay* md)
{
    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);
    if (fd.lat_deg == 0.0 && fd.lon_deg == 0.0) return;
    if (md->disp_lat == 0.0 && md->disp_lon == 0.0) {
        md->disp_lat = fd.lat_deg; md->disp_lon = fd.lon_deg; return;
    }
    md->disp_lat += (fd.lat_deg - md->disp_lat) * CABIN_POS_EASE;
    md->disp_lon += (fd.lon_deg - md->disp_lon) * CABIN_POS_EASE;
}

static void drain_pending(MapDisplay* md)
{
    SDL_LockMutex(md->pending_mutex);
    for (int i = 0; i < md->pending_count; i++) {
        if (md->pending[i].surf) {
            tile_cache_put(md->tile_cache, md->pending[i].key, md->pending[i].surf);
            md->pending[i].surf = NULL;
        }
    }
    md->pending_count = 0;
    SDL_UnlockMutex(md->pending_mutex);
}

static void trigger_fetch(MapDisplay* md)
{
    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);
    if (fd.lat_deg == 0.0 && fd.lon_deg == 0.0) return;
    if (!SDL_AtomicGet(&md->fetch_pending)) {
        SDL_LockMutex(md->fetch_mutex);
        md->fetch_lat = md->disp_lat; md->fetch_lon = md->disp_lon;
        md->fetch_zoom = md->zoom;
        SDL_AtomicSet(&md->fetch_pending, 1);
        SDL_UnlockMutex(md->fetch_mutex);
    }
}

static void update_auto_zoom(MapDisplay* md)
{
    if (md->base_zoom <= 0.0) return;
    double elapsed = (double)(SDL_GetTicks64() - md->zoom_start_ms) / 1000.0;
    double phase = fmod(elapsed, 60.0) / 60.0;
    int z = (int)(md->base_zoom + sin(phase * M_PI * 2.0) * 2.5 + 0.5);
    if (z < CABIN_ZOOM_MIN) z = CABIN_ZOOM_MIN;
    if (z > CABIN_ZOOM_MAX) z = CABIN_ZOOM_MAX;
    md->zoom = z;
}

/* =========================================================================
 *  Render functions
 * ========================================================================= */

static void render_tiles(MapDisplay* md)
{
    SDL_Renderer* r = md->renderer;
    SDL_Rect* mr = &md->map_rect;

    SDL_RenderSetClipRect(r, mr);
    SDL_SetRenderDrawColor(r, 13, 26, 43, 255);
    SDL_RenderFillRect(r, mr);

    double clat = md->disp_lat, clon = md->disp_lon;
    int zoom = md->zoom;
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
            SDL_Texture* tex = tile_cache_get(md->tile_cache, key);
            if (!tex) continue;
            int sx = tx * GEO_TILE_SIZE - (cxi * GEO_TILE_SIZE + cpx) + scx;
            int sy = ty * GEO_TILE_SIZE - (cyi * GEO_TILE_SIZE + cpy) + scy;
            SDL_Rect dst = { sx, sy, GEO_TILE_SIZE, GEO_TILE_SIZE };
            SDL_RenderCopy(r, tex, NULL, &dst);
        }
    }
    SDL_RenderSetClipRect(r, NULL);
}

static void render_header(MapDisplay* md)
{
    SDL_Renderer* r = md->renderer;
    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);

    SDL_Rect hdr = { 0, 0, md->win_w, CABIN_HEADER_H };
    SDL_SetRenderDrawColor(r, 10, 30, 50, 255);
    SDL_RenderFillRect(r, &hdr);
    SDL_SetRenderDrawColor(r, 42, 106, 172, 255);
    SDL_Rect line = { 0, CABIN_HEADER_H - 2, md->win_w, 2 };
    SDL_RenderFillRect(r, &line);

    char buf[64] = "----  →  ----";
    if (md->fmc && md->fmc->flight_plan.waypoint_count >= 2)
        snprintf(buf, sizeof(buf), "%s  →  %s",
                 md->fmc->flight_plan.departure.icao,
                 md->fmc->flight_plan.arrival.icao);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    font_draw(r, md->win_w / 2, CABIN_HEADER_H / 2, buf, 12, FONT_BOLD);
}

static void render_data_bar(MapDisplay* md)
{
    SDL_Renderer* r = md->renderer;
    int bar_y = md->win_h - CABIN_DATABAR_H;
    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);

    double flown, total, remain, pct;
    calc_progress(&fd, md->fmc, &flown, &total, &remain, &pct);

    SDL_Rect bar = { 0, bar_y, md->win_w, CABIN_DATABAR_H };
    SDL_SetRenderDrawColor(r, 10, 30, 50, 240);
    SDL_RenderFillRect(r, &bar);
    SDL_SetRenderDrawColor(r, 42, 106, 172, 255);
    SDL_Rect top = { 0, bar_y, md->win_w, 2 };
    SDL_RenderFillRect(r, &top);

    int col_w = md->win_w / 7;
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

static void render_progress(MapDisplay* md)
{
    SDL_Renderer* r = md->renderer;
    int prog_y = md->win_h - CABIN_DATABAR_H - CABIN_PROGRESS_H;
    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);

    double flown, total, remain, pct;
    calc_progress(&fd, md->fmc, &flown, &total, &remain, &pct);

    SDL_Rect track = { 0, prog_y, md->win_w, CABIN_PROGRESS_H };
    SDL_SetRenderDrawColor(r, 255, 255, 255, 20);
    SDL_RenderFillRect(r, &track);

    int fw = (int)((double)md->win_w * pct / 100.0);
    if (fw < 0) fw = 0; if (fw > md->win_w) fw = md->win_w;
    SDL_Rect fill = { 0, prog_y, fw, CABIN_PROGRESS_H };
    SDL_SetRenderDrawColor(r, 26, 115, 232, 255);
    SDL_RenderFillRect(r, &fill);
}

/* =========================================================================
 *  Public render entry
 * ========================================================================= */

void map_display_render(MapDisplay* md)
{
    if (!md || !md->renderer) return;

    /* Update window size each frame (avoids event-polling conflicts with main loop) */
    SDL_GetWindowSize(md->window, &md->win_w, &md->win_h);
    md->map_rect.w = md->win_w;
    md->map_rect.h = md->win_h - CABIN_HEADER_H - CABIN_DATABAR_H - CABIN_PROGRESS_H;
    if (md->map_rect.h < 40) md->map_rect.h = 40;

    drain_pending(md);
    interpolate(md);
    update_auto_zoom(md);

    /* Route change detection → reset base zoom */
    if (md->fmc && md->fmc->flight_plan.waypoint_count >= 2) {
        const FlightPlan* fp = &md->fmc->flight_plan;
        if (fp->waypoint_count != md->last_wpt_count ||
            strcmp(fp->departure.icao, md->last_dep_icao) != 0 ||
            strcmp(fp->arrival.icao, md->last_arr_icao) != 0) {
            md->last_wpt_count = fp->waypoint_count;
            strncpy(md->last_dep_icao, fp->departure.icao, 7);
            strncpy(md->last_arr_icao, fp->arrival.icao, 7);
            md->base_zoom = (double)md->zoom;
            md->zoom_start_ms = SDL_GetTicks64();
        }
    }

    trigger_fetch(md);

    /* Render to cabin window */
    SDL_SetRenderDrawColor(md->renderer, 13, 26, 43, 255);
    SDL_RenderClear(md->renderer);

    render_tiles(md);

    /* Trajectory overlay — use interpolated position for aircraft */
    FlightDataValues fd;
    SDL_LockMutex(md->data_mutex); fd = md->last_fd; SDL_UnlockMutex(md->data_mutex);
    fd.lat_deg = md->disp_lat;
    fd.lon_deg = md->disp_lon;
    trajectory_render(md->renderer, md->disp_lat, md->disp_lon, md->zoom,
                      md->map_rect.w, md->map_rect.h,
                      md->map_rect.x, md->map_rect.y,
                      &fd, md->fmc);

    render_header(md);
    render_progress(md);
    render_data_bar(md);

    SDL_RenderPresent(md->renderer);
}
