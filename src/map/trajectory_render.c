/**
 * @file    trajectory_render.c
 * @brief   SDL2 trajectory overlay implementation.
 *
 * Draw order (back to front):
 *   1. Planned route — dashed yellow polyline
 *   2. Flown path — solid blue polyline
 *   3. Waypoint markers — dots + labels
 *   4. Departure/arrival markers
 *   5. Aircraft icon
 */

#include "trajectory_render.h"
#include "geo_projection.h"
#include "data/flight_data.h"
#include "data/navdata.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 *  Color constants (RGBA)
 * ========================================================================= */

#define COL_PLANNED_R     0xFF
#define COL_PLANNED_G     0xCC
#define COL_PLANNED_B     0x00
#define COL_PLANNED_A     0xFF

#define COL_FLOWN_R       0x1A
#define COL_FLOWN_G       0x73
#define COL_FLOWN_B       0xE8
#define COL_FLOWN_A       0xFF

#define COL_DEP_R         0x27
#define COL_DEP_G         0xAE
#define COL_DEP_B         0x60
#define COL_DEP_A         0xFF

#define COL_ARR_R         0xE7
#define COL_ARR_G         0x4C
#define COL_ARR_B         0x3C
#define COL_ARR_A         0xFF

#define COL_WPT_R         0x33
#define COL_WPT_G         0x55
#define COL_WPT_B         0x77
#define COL_WPT_A         0xFF

#define COL_AIRCRAFT_R    0x1A
#define COL_AIRCRAFT_G    0x73
#define COL_AIRCRAFT_B    0xE8
#define COL_AIRCRAFT_A    0xFF

#define COL_SHADOW_R      0x00
#define COL_SHADOW_G      0x00
#define COL_SHADOW_B      0x00
#define COL_SHADOW_A      0x64

/* =========================================================================
 *  Internal helpers
 * ========================================================================= */

/**
 * @brief Convert a waypoint to screen pixel coordinates.
 */
static void wpt_to_screen(const GeoPos* pos,
                          double center_lat, double center_lon,
                          int zoom, int map_w, int map_h,
                          int map_ox, int map_oy,
                          int* px, int* py)
{
    geo_to_screen(pos->lat_deg, pos->lon_deg,
                  center_lat, center_lon, zoom, map_w, map_h, px, py);
    *px += map_ox;
    *py += map_oy;
}

/**
 * @brief Draw a filled circle (approximated with lines at varying radii).
 */
static void draw_filled_circle(SDL_Renderer* r, int cx, int cy, int radius)
{
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrtf((float)(radius * radius - dy * dy));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

/**
 * @brief Draw a circle outline.
 */
static void draw_circle_outline(SDL_Renderer* r, int cx, int cy,
                                int radius, int thickness)
{
    for (int t = 0; t < thickness; t++) {
        int rr = radius + t;
        int x = rr;
        int y = 0;
        int err = 1 - rr;
        while (x >= y) {
            SDL_RenderDrawPoint(r, cx + x, cy + y);
            SDL_RenderDrawPoint(r, cx - x, cy + y);
            SDL_RenderDrawPoint(r, cx + x, cy - y);
            SDL_RenderDrawPoint(r, cx - x, cy - y);
            SDL_RenderDrawPoint(r, cx + y, cy + x);
            SDL_RenderDrawPoint(r, cx - y, cy + x);
            SDL_RenderDrawPoint(r, cx + y, cy - x);
            SDL_RenderDrawPoint(r, cx - y, cy - x);
            y++;
            if (err <= 0) { err += 2 * y + 1; }
            else { x--; err += 2 * (y - x) + 1; }
        }
    }
}

/* =========================================================================
 *  Dashed line
 * ========================================================================= */

void trajectory_draw_dashed_line(SDL_Renderer* r,
                                 int x1, int y1, int x2, int y2,
                                 int dash_len, int gap_len)
{
    double dx = (double)(x2 - x1);
    double dy = (double)(y2 - y1);
    double dist = sqrt(dx * dx + dy * dy);
    if (dist < 1.0) return;

    double ux = dx / dist;
    double uy = dy / dist;
    double pos = 0.0;
    int on = 1;

    while (pos < dist) {
        double end = pos + (on ? (double)dash_len : (double)gap_len);
        if (end > dist) end = dist;
        if (on) {
            SDL_RenderDrawLine(r,
                (int)((double)x1 + ux * pos), (int)((double)y1 + uy * pos),
                (int)((double)x1 + ux * end), (int)((double)y1 + uy * end));
        }
        pos = end;
        on = !on;
    }
}

/* =========================================================================
 *  Aircraft icon
 * ========================================================================= */

void trajectory_draw_aircraft(SDL_Renderer* r, int cx, int cy,
                              float heading, float scale)
{
    /* Aircraft triangle vertices (nose up, pre-rotation):
     * Nose: (0, -15), Wing left: (-8, 8), Wing right: (8, 8)
     * Scale factor applied */
    float vx[3] = { 0.0f,  -8.0f * scale,  8.0f * scale };
    float vy[3] = { -15.0f * scale,  8.0f * scale,  8.0f * scale };

    float hdg_rad = (float)GEO_DEG2RAD((double)heading);
    float s = sinf(hdg_rad);
    float c = cosf(hdg_rad);

    SDL_Point pts[4];
    for (int i = 0; i < 3; i++) {
        float rx = vx[i] * c - vy[i] * s;
        float ry = vx[i] * s + vy[i] * c;
        pts[i].x = cx + (int)rx;
        pts[i].y = cy + (int)ry;
    }
    pts[3] = pts[0];  /* close the polygon */

    /* Shadow */
    SDL_SetRenderDrawColor(r, COL_SHADOW_R, COL_SHADOW_G,
                           COL_SHADOW_B, COL_SHADOW_A);
    for (int dy = 1; dy <= 3; dy++) {
        for (int i = 0; i < 3; i++) {
            SDL_RenderDrawLine(r,
                pts[i].x + dy, pts[i].y + dy,
                pts[i+1].x + dy, pts[i+1].y + dy);
        }
    }

    /* Fill the triangle using horizontal scanlines */
    SDL_SetRenderDrawColor(r, COL_AIRCRAFT_R, COL_AIRCRAFT_G,
                           COL_AIRCRAFT_B, COL_AIRCRAFT_A);
    int min_y = pts[0].y, max_y = pts[0].y;
    for (int i = 1; i < 3; i++) {
        if (pts[i].y < min_y) min_y = pts[i].y;
        if (pts[i].y > max_y) max_y = pts[i].y;
    }
    for (int y = min_y; y <= max_y; y++) {
        /* Find x intersections with the three edges */
        int xs[6];
        int nx = 0;
        for (int i = 0; i < 3; i++) {
            int y1 = pts[i].y, y2 = pts[i+1].y;
            if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
                int x1 = pts[i].x, x2 = pts[i+1].x;
                int x = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
                xs[nx++] = x;
            }
        }
        if (nx >= 2) {
            if (xs[0] > xs[1]) { int t = xs[0]; xs[0] = xs[1]; xs[1] = t; }
            SDL_RenderDrawLine(r, xs[0], y, xs[1], y);
        }
    }

    /* Bold outline */
    SDL_SetRenderDrawColor(r, 255, 255, 255, 180);
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < 3; i++) {
            SDL_RenderDrawLine(r,
                pts[i].x + t, pts[i].y,
                pts[i+1].x + t, pts[i+1].y);
        }
    }
}

/* =========================================================================
 *  Main trajectory render
 * ========================================================================= */

void trajectory_render(SDL_Renderer* r,
                       double center_lat, double center_lon,
                       int zoom, int map_w, int map_h,
                       int map_ox, int map_oy,
                       const FlightDataValues* fd,
                       const FMCState* fmc)
{
    if (!r || !fd) return;

    /* If no FMC or no flight plan, just draw the aircraft if we have position */
    if (!fmc || fmc->flight_plan.waypoint_count < 2) {
        if (fd->lat_deg != 0.0 || fd->lon_deg != 0.0) {
            int ax, ay;
            geo_to_screen(fd->lat_deg, fd->lon_deg,
                         center_lat, center_lon, zoom, map_w, map_h, &ax, &ay);
            ax += map_ox;
            ay += map_oy;
            trajectory_draw_aircraft(r, ax, ay, fd->heading_true_deg, 1.0f);
        }
        return;
    }

    const FlightPlan* fp = &fmc->flight_plan;
    int n = fp->waypoint_count;

    /* Collect screen coordinates for all waypoints */
    int* sx = (int*)malloc((size_t)n * sizeof(int));
    int* sy = (int*)malloc((size_t)n * sizeof(int));
    if (!sx || !sy) {
        free(sx);
        free(sy);
        return;
    }

    for (int i = 0; i < n; i++) {
        wpt_to_screen(&fp->waypoints[i].pos,
                      center_lat, center_lon, zoom, map_w, map_h,
                      map_ox, map_oy, &sx[i], &sy[i]);
    }

    /* 1. Planned route — dashed yellow polyline */
    SDL_SetRenderDrawColor(r, COL_PLANNED_R, COL_PLANNED_G,
                           COL_PLANNED_B, COL_PLANNED_A);
    for (int i = 0; i < n - 1; i++) {
        trajectory_draw_dashed_line(r, sx[i], sy[i], sx[i+1], sy[i+1], 10, 10);
    }

    /* 2. Flown path — solid blue polyline */
    int active_idx = fp->active_waypoint_index;
    if (active_idx > 0 && active_idx < n) {
        SDL_SetRenderDrawColor(r, COL_FLOWN_R, COL_FLOWN_G,
                               COL_FLOWN_B, COL_FLOWN_A);
        for (int i = 0; i < active_idx; i++) {
            /* Thick lines: draw 3 adjacent lines */
            for (int t = -1; t <= 1; t++) {
                SDL_RenderDrawLine(r, sx[i], sy[i] + t, sx[i+1], sy[i+1] + t);
            }
        }
        /* Draw line from last flown waypoint to current aircraft position */
        int ax, ay;
        geo_to_screen(fd->lat_deg, fd->lon_deg,
                     center_lat, center_lon, zoom, map_w, map_h, &ax, &ay);
        ax += map_ox;
        ay += map_oy;
        for (int t = -1; t <= 1; t++) {
            SDL_RenderDrawLine(r, sx[active_idx], sy[active_idx] + t, ax, ay + t);
        }
    }

    /* 3. Waypoint markers (intermediate, excluding first and last) */
    SDL_SetRenderDrawColor(r, COL_WPT_R, COL_WPT_G, COL_WPT_B, COL_WPT_A);
    for (int i = 1; i < n - 1; i++) {
        /* Tiny dot */
        draw_filled_circle(r, sx[i], sy[i], 3);
        draw_circle_outline(r, sx[i], sy[i], 3, 1);
    }

    /* 4. Departure marker (green) */
    SDL_SetRenderDrawColor(r, COL_DEP_R, COL_DEP_G, COL_DEP_B, COL_DEP_A);
    draw_filled_circle(r, sx[0], sy[0], 8);
    draw_circle_outline(r, sx[0], sy[0], 8, 2);

    /* 5. Arrival marker (red) */
    SDL_SetRenderDrawColor(r, COL_ARR_R, COL_ARR_G, COL_ARR_B, COL_ARR_A);
    draw_filled_circle(r, sx[n-1], sy[n-1], 8);
    draw_circle_outline(r, sx[n-1], sy[n-1], 8, 2);

    /* 6. Aircraft icon at current position */
    if (fd->lat_deg != 0.0 || fd->lon_deg != 0.0) {
        int ax, ay;
        geo_to_screen(fd->lat_deg, fd->lon_deg,
                     center_lat, center_lon, zoom, map_w, map_h, &ax, &ay);
        ax += map_ox;
        ay += map_oy;
        trajectory_draw_aircraft(r, ax, ay, fd->heading_true_deg, 1.0f);
    }

    free(sx);
    free(sy);
}
