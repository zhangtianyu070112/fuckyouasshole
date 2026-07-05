/**
 * @file    nd.c
 * @brief   Navigation Display implementation.
 *
 * Supported modes:
 *   - ROSE: Full 360° compass rose (default)
 *   - ARC:  120° forward arc (not yet implemented)
 *
 * Range: 10, 20, 40, 80, 160, 320 NM (selectable)
 */

#include "nd.h"
#include "app.h"
#include "data/flight_data.h"
#include "data/navdata.h"
#include "utils/math_util.h"
#include "utils/font_manager.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* === Colors (same palette as PFD) === */
#define COL_BG         0x10, 0x10, 0x18, 255
#define COL_TAPE_BG    0x20, 0x20, 0x28, 255
#define COL_WHITE      0xFF, 0xFF, 0xFF, 255
#define COL_GREEN      0x00, 0xFF, 0x40, 255
#define COL_AMBER      0xFF, 0xC0, 0x00, 255
#define COL_CYAN       0x00, 0xFF, 0xFF, 255
#define COL_MAGENTA    0xFF, 0x00, 0xFF, 255
#define COL_GRAY       0x60, 0x60, 0x68, 255
#define COL_DARK_GRAY  0x30, 0x30, 0x38, 255

typedef struct {
    int    mode;          /* 0=ROSE, 1=ARC */
    int    range_nm;      /* Current range setting */
    float  smooth_hdg;    /* Smoothed heading for display */
    FMCState* fmc;       /* FMC state for route display */
    FlightDataValues fd;  /* Latest flight data snapshot */

    /* Display toggles */
    int    show_waypoints; /* 1 = draw nav waypoints */
    int    show_airports;  /* 1 = draw nav airports */
    int    show_route;     /* 1 = draw active flight plan route */

    /* Ground track computation (from GPS position delta) */
    float  prev_lat, prev_lon;     /* Previous frame position */
    float  track_true_deg;         /* Computed true track (°) */
    float  track_mag_deg;          /* Computed magnetic track (°) */
    int    track_valid;            /* 1 = valid track from position movement */
} NDData;

static void set_col(SDL_Renderer* r, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    SDL_SetRenderDrawColor(r, R, G, B, A);
}

/* =========================================================================
 *  geo_to_screen — convert lat/lon to compass-rose pixel coordinates
 * ========================================================================= */

/**
 * @brief Convert a geographic position to screen coords on the ND.
 * @param ac     Aircraft position (lat/lon).
 * @param hdg    Aircraft true heading (degrees).
 * @param px_nm  Pixels per nautical mile (radius / range_nm).
 * @param cx,cy  Compass rose center on screen.
 * @param wpt    Target waypoint/airport position.
 * @param out_x,out_y  Output pixel coords.
 * @return 1 if within display range, 0 if too far (coords still set to edge).
 */
static int geo_to_screen(GeoPos ac, float hdg, float px_nm,
                         int cx, int cy, GeoPos wpt,
                         int* out_x, int* out_y, float range_nm, int radius)
{
    double dist_nm = geo_distance_nm(ac, wpt);
    double brg_deg = geo_bearing_deg(ac, wpt);

    float px_dist = (float)dist_nm * px_nm;

    /* Same angle convention as compass ticks: angle = brg - hdg - 90 */
    float angle = (float)DEG2RAD(brg_deg - (double)hdg - 90.0);

    int visible = (dist_nm <= (double)range_nm * 1.1);
    if (px_dist > (float)radius) px_dist = (float)radius - 4.0f;

    *out_x = cx + (int)(px_dist * cosf(angle));
    *out_y = cy + (int)(px_dist * sinf(angle));
    return visible;
}

/* =========================================================================
 *  Top bar — TRK|MAG + active waypoint info
 * ========================================================================= */

static void draw_nd_top_bar(SDL_Renderer* r, const SDL_Rect* rect, NDData* nd,
                             const FlightDataValues* f)
{
    int x0 = rect->x + 8;
    int y0 = rect->y + 4;

    /* ---- Semi-transparent background strip ---- */
    set_col(r, 0x08, 0x08, 0x10, 200);
    SDL_Rect bar = { rect->x, rect->y, rect->w, 38 };
    SDL_RenderFillRect(r, &bar);

    /* Thin separator line below the bar */
    set_col(r, COL_GRAY);
    SDL_RenderDrawLine(r, rect->x, rect->y + 38, rect->x + rect->w, rect->y + 38);

    /* === Left side: TRK | MAG === */
    float track = nd->track_valid ? nd->track_mag_deg : f->heading_mag_deg;
    /* When GS < 30 kt (near-stationary), track is unreliable — fall back to heading */
    if (f->gs_kts < 30.0f) {
        track = f->heading_mag_deg;
    }

    /* "TRK" label in small cyan */
    set_col(r, COL_CYAN);
    draw_text_left(r, x0, y0 + 2, "TRK", 0.5f);

    /* Track value in large white */
    char trk_str[8];
    snprintf(trk_str, sizeof(trk_str), "%.0f", (double)track);
    set_col(r, COL_WHITE);
    draw_text_left(r, x0 + 26, y0 + 2, trk_str, 0.75f);

    /* Degree symbol + "MAG" indicator in green */
    set_col(r, COL_GREEN);
    draw_text_left(r, x0 + 60, y0 + 6, "MAG", 0.4f);

    /* === Right side: Active waypoint info (目标航路) === */
    if (nd->fmc) {
        FlightPlan* fp = &nd->fmc->flight_plan;
        if (fp->waypoint_count > 0 && fp->active_waypoint_index >= 0
            && fp->active_waypoint_index < fp->waypoint_count) {

            const Waypoint* wpt = &fp->waypoints[fp->active_waypoint_index];

            /* Build aircraft position (default to Beijing area if unset) */
            GeoPos ac;
            ac.lat_deg = f->lat_deg;
            ac.lon_deg = f->lon_deg;
            if (fabs(ac.lat_deg) < 0.01 && fabs(ac.lon_deg) < 0.01) {
                ac.lat_deg = 39.0;
                ac.lon_deg = 117.0;
            }

            double dist_nm = geo_distance_nm(ac, wpt->pos);

            /* ETA: distance / ground-speed → minutes */
            int eta_total_min = 0;
            if (f->gs_kts > 30.0f) {
                eta_total_min = (int)(dist_nm / (double)f->gs_kts * 60.0 + 0.5);
            }

            int rx = rect->x + rect->w - 12;

            /* Waypoint identifier — magenta, large */
            set_col(r, COL_MAGENTA);
            font_draw_scaled_aligned(r, rx, y0 + 1, wpt->ident,
                                     0.65f, FONT_BOLD, FONT_ALIGN_RIGHT);

            /* Distance in NM — white */
            char dist_str[32];
            snprintf(dist_str, sizeof(dist_str), "%.0f NM", dist_nm);
            set_col(r, COL_WHITE);
            font_draw_scaled_aligned(r, rx, y0 + 17, dist_str,
                                     0.5f, FONT_REGULAR, FONT_ALIGN_RIGHT);

            /* ETA — green (HH:MM format) */
            char eta_str[12];
            int hrs = eta_total_min / 60;
            int min = eta_total_min % 60;
            if (hrs > 0)
                snprintf(eta_str, sizeof(eta_str), "%d:%02d", hrs, min);
            else
                snprintf(eta_str, sizeof(eta_str), ":%02d", min);
            set_col(r, COL_GREEN);
            font_draw_scaled_aligned(r, rx, y0 + 28, eta_str,
                                     0.45f, FONT_REGULAR, FONT_ALIGN_RIGHT);
        }
    }
}

/* =========================================================================
 *  Compass rose (ROSE mode)
 * ========================================================================= */

static void draw_compass_rose(SDL_Renderer* r, const SDL_Rect* rect, NDData* nd,
                              const FlightDataValues* f)
{
    int cx   = rect->x + rect->w * 64 / 100;  /* Offset left for data panel */
    int cy   = rect->y + rect->h * 48 / 100;
    int radius = (int)((float)(rect->h < rect->w ? rect->h : rect->w) * 0.38f);
    if (radius < 40) radius = 40;

    float hdg = f->heading_true_deg;

    /* --- Range rings --- */
    float px_per_nm = (float)radius / (float)nd->range_nm;

    int rings[] = { nd->range_nm / 4, nd->range_nm / 2, nd->range_nm * 3 / 4, nd->range_nm };
    for (int ri = 0; ri < 4; ri++) {
        int r_px = (int)((float)rings[ri] * px_per_nm);
        if (r_px > radius) r_px = radius;
        set_col(r, COL_GRAY);
        /* Draw circle using line segments */
        int pts = 64;
        for (int i = 0; i < pts; i++) {
            float a1 = (float)i * 2.0f * (float)M_PI / (float)pts;
            float a2 = (float)(i + 1) * 2.0f * (float)M_PI / (float)pts;
            SDL_RenderDrawLine(r,
                               cx + (int)((float)r_px * cosf(a1)),
                               cy + (int)((float)r_px * sinf(a1)),
                               cx + (int)((float)r_px * cosf(a2)),
                               cy + (int)((float)r_px * sinf(a2)));
        }

        /* Range text */
        char rng[8];
        snprintf(rng, sizeof(rng), "%d", rings[ri]);
        set_col(r, COL_CYAN);
        /* Use simple line-based text placeholder */
        SDL_RenderDrawLine(r, cx + r_px, cy, cx + r_px + 2, cy);
    }

    /* --- Compass ticks & labels --- */
    for (int deg = 0; deg < 360; deg += 5) {
        float angle_rad = (float)DEG2RAD((double)deg - (double)hdg - 90.0);
        float c = cosf(angle_rad);
        float s = sinf(angle_rad);
        float outer = (float)radius;
        float inner;

        int is_cardinal = (deg % 90 == 0);
        int is_10 = (deg % 10 == 0);

        if (is_cardinal) inner = outer - 22.0f;
        else if (is_10)  inner = outer - 16.0f;
        else             inner = outer - 10.0f;

        set_col(r, COL_WHITE);
        SDL_RenderDrawLine(r,
                           cx + (int)(inner * c), cy + (int)(inner * s),
                           cx + (int)(outer * c), cy + (int)(outer * s));

        /* Cardinal labels */
        if (is_cardinal) {
            const char* lbl;
            switch (deg) {
                case 0:   lbl = "N"; break;
                case 90:  lbl = "E"; break;
                case 180: lbl = "S"; break;
                case 270: lbl = "W"; break;
                default:  lbl = "";  break;
            }
            /* Cardinal labels using TTF */
            float lx = (outer + 18.0f) * c;
            float ly = (outer + 18.0f) * s;
            set_col(r, COL_CYAN);
            draw_text_simple(r, cx + (int)lx, cy + (int)ly, lbl, 0.55f);
        }
    }

    /* --- Heading bug (selected heading) --- */
    float ap_hdg_rad = (float)DEG2RAD((double)f->ap_hdg - (double)hdg - 90.0);
    float bug_x = (float)radius * cosf(ap_hdg_rad);
    float bug_y = (float)radius * sinf(ap_hdg_rad);
    set_col(r, COL_CYAN);
    /* Draw a small diamond for the heading bug */
    SDL_RenderDrawLine(r, cx + (int)bug_x,     cy + (int)bug_y - 5,
                       cx + (int)bug_x + 5, cy + (int)bug_y);
    SDL_RenderDrawLine(r, cx + (int)bug_x + 5, cy + (int)bug_y,
                       cx + (int)bug_x,     cy + (int)bug_y + 5);
    SDL_RenderDrawLine(r, cx + (int)bug_x,     cy + (int)bug_y + 5,
                       cx + (int)bug_x - 5, cy + (int)bug_y);
    SDL_RenderDrawLine(r, cx + (int)bug_x - 5, cy + (int)bug_y,
                       cx + (int)bug_x,     cy + (int)bug_y - 5);

    /* --- Aircraft symbol (center) --- */
    set_col(r, COL_WHITE);
    /* Fuselage */
    SDL_RenderDrawLine(r, cx, cy - 12, cx, cy + 12);
    /* Wings */
    SDL_RenderDrawLine(r, cx - 15, cy - 3, cx + 15, cy - 3);
    /* Tail */
    SDL_RenderDrawLine(r, cx - 5, cy + 6, cx + 5, cy + 6);
    /* Dot */
    draw_filled_circle(r, cx, cy, 3);

    /* --- Track line (if moving) --- */
    if (f->gs_kts > 30.0f) {
        float trk_rad = (float)DEG2RAD(-90.0);  /* Straight ahead is -90° in screen coords */
        set_col(r, COL_GREEN);
        SDL_RenderDrawLine(r, cx, cy, cx + (int)((float)radius * cosf(trk_rad)),
                           cy + (int)((float)radius * sinf(trk_rad)));
    }

    /* --- Wind vector --- */
    if (f->wind_speed_kts > 2.0f) {
        float wind_scale = 2.0f;  /* pixels per knot */
        float wind_rad = (float)DEG2RAD((double)f->wind_dir_deg - (double)hdg - 90.0);
        int wx = (int)((float)cx + (float)radius * 0.5f * cosf(wind_rad));
        int wy = (int)((float)cy + (float)radius * 0.5f * sinf(wind_rad));
        int wl = (int)(f->wind_speed_kts * wind_scale);
        /* Arrow in wind direction */
        float arrow_rad = (float)DEG2RAD((double)f->wind_dir_deg + 180.0 - (double)hdg - 90.0);
        set_col(r, COL_WHITE);
        SDL_RenderDrawLine(r, wx, wy,
                           wx + (int)((float)wl * cosf(arrow_rad)),
                           wy + (int)((float)wl * sinf(arrow_rad)));
        /* Arrowhead */
        float head_a = arrow_rad + 0.6f;
        float head_b = arrow_rad - 0.6f;
        int ax = wx + (int)((float)wl * cosf(arrow_rad));
        int ay = wy + (int)((float)wl * sinf(arrow_rad));
        SDL_RenderDrawLine(r, ax, ay,
                           ax - (int)(8.0f * cosf(head_a)),
                           ay - (int)(8.0f * sinf(head_a)));
        SDL_RenderDrawLine(r, ax, ay,
                           ax - (int)(8.0f * cosf(head_b)),
                           ay - (int)(8.0f * sinf(head_b)));

        /* Wind speed label next to arrowhead */
        char wspd[8];
        snprintf(wspd, sizeof(wspd), "%.0f", (double)f->wind_speed_kts);
        set_col(r, COL_WHITE);
        draw_text_simple(r, ax + 6, ay - 8, wspd, 0.35f);
    }

    /* --- Route / waypoint / airport overlay --- */

    /* Aircraft position: use flight data or default to Beijing area */
    GeoPos ac;
    ac.lat_deg = f->lat_deg;
    ac.lon_deg = f->lon_deg;
    if (fabs(ac.lat_deg) < 0.01 && fabs(ac.lon_deg) < 0.01) {
        ac.lat_deg = 39.0;   /* Default: near Beijing */
        ac.lon_deg = 117.0;
    }

    float px_nm = (float)radius / (float)nd->range_nm;

    /* --- Nav waypoints (cyan diamonds) --- */
    if (nd->show_waypoints && nd->fmc) {
        for (int i = 0; i < nd->fmc->nav_wpt_count; i++) {
            const Waypoint* w = &nd->fmc->nav_waypoints[i];
            int sx, sy;
            int vis = geo_to_screen(ac, hdg, px_nm, cx, cy, w->pos,
                                    &sx, &sy, (float)nd->range_nm, radius);
            /* Diamond marker */
            set_col(r, COL_CYAN);
            int d = 3;
            SDL_RenderDrawLine(r, sx - d, sy, sx, sy - d);
            SDL_RenderDrawLine(r, sx, sy - d, sx + d, sy);
            SDL_RenderDrawLine(r, sx + d, sy, sx, sy + d);
            SDL_RenderDrawLine(r, sx, sy + d, sx - d, sy);
            /* Label (only at close ranges, avoid clutter) */
            if (vis && nd->range_nm <= 80) {
                set_col(r, COL_CYAN);
                font_draw_scaled_aligned(r, sx + 5, sy - 4, w->ident,
                                         0.4f, FONT_REGULAR, FONT_ALIGN_LEFT);
            }
        }
    }

    /* --- Airports (magenta squares) --- */
    if (nd->show_airports && nd->fmc) {
        for (int i = 0; i < nd->fmc->nav_apt_count; i++) {
            const Airport* a = &nd->fmc->nav_airports[i];
            int sx, sy;
            int vis = geo_to_screen(ac, hdg, px_nm, cx, cy, a->pos,
                                    &sx, &sy, (float)nd->range_nm, radius);
            set_col(r, COL_MAGENTA);
            int s = 4;
            SDL_Rect sq = { sx - s, sy - s, s * 2, s * 2 };
            SDL_RenderDrawRect(r, &sq);
            if (vis && nd->range_nm <= 80) {
                set_col(r, COL_MAGENTA);
                font_draw_scaled_aligned(r, sx + 5, sy - 4, a->icao,
                                         0.4f, FONT_REGULAR, FONT_ALIGN_LEFT);
            }
        }
    }

    /* --- Active flight plan route (green lines + white dots) --- */
    if (nd->show_route && nd->fmc) {
        FlightPlan* fp = &nd->fmc->flight_plan;
        if (fp->waypoint_count >= 2) {
            for (int i = 0; i < fp->waypoint_count - 1; i++) {
                int x1, y1, x2, y2;
                int v1 = geo_to_screen(ac, hdg, px_nm, cx, cy,
                                       fp->waypoints[i].pos,
                                       &x1, &y1, (float)nd->range_nm, radius);
                int v2 = geo_to_screen(ac, hdg, px_nm, cx, cy,
                                       fp->waypoints[i + 1].pos,
                                       &x2, &y2, (float)nd->range_nm, radius);
                /* Active leg: magenta, others: green */
                int is_active = (i == fp->active_waypoint_index - 1);
                set_col(r, is_active ? COL_MAGENTA : COL_GREEN);
                int lw = is_active ? 3 : 2;
                draw_thick_line(r, x1, y1, x2, y2, lw);
            }
        }
        /* Waypoint dots on route */
        for (int i = 0; i < fp->waypoint_count; i++) {
            int sx, sy;
            geo_to_screen(ac, hdg, px_nm, cx, cy,
                          fp->waypoints[i].pos,
                          &sx, &sy, (float)nd->range_nm, radius);
            int is_active = (i == fp->active_waypoint_index);
            set_col(r, is_active ? COL_MAGENTA : COL_WHITE);
            draw_filled_circle(r, sx, sy, is_active ? 4 : 3);
        }
    }
}

/* =========================================================================
 *  Left data panel
 * ========================================================================= */

static void draw_nd_data_panel(SDL_Renderer* r, const SDL_Rect* rect,
                               const FlightDataValues* f, NDData* nd)
{
    int x0 = rect->x + 8;
    int y  = rect->y + 48;   /* Start below the 38px top bar */
    int line_h = 22;

    /* Ground speed */
    set_col(r, COL_GREEN);
    draw_text_left(r, x0, y, "GS", 0.6f);
    char gs_str[16];
    snprintf(gs_str, sizeof(gs_str), "%.0f kt", (double)f->gs_kts);
    draw_text_left(r, x0 + 24, y, gs_str, 0.7f);
    y += line_h;

    /* True airspeed */
    set_col(r, COL_GREEN);
    draw_text_left(r, x0, y, "TAS", 0.6f);
    char tas_str[16];
    snprintf(tas_str, sizeof(tas_str), "%.0f kt", (double)f->tas_kts);
    draw_text_left(r, x0 + 24, y, tas_str, 0.7f);
    y += line_h;

    /* Wind */
    set_col(r, COL_AMBER);
    char wind_str[32];
    snprintf(wind_str, sizeof(wind_str), "%.0f°/%.0f kt",
             (double)f->wind_dir_deg, (double)f->wind_speed_kts);
    draw_text_left(r, x0, y, wind_str, 0.6f);
    y += line_h;

    /* OAT */
    set_col(r, COL_CYAN);
    char oat_str[16];
    snprintf(oat_str, sizeof(oat_str), "OAT %+.0f°C", (double)f->oat_c);
    draw_text_left(r, x0, y, oat_str, 0.55f);
    y += line_h;

    /* COM1 */
    {
        char com1_str[20];
        snprintf(com1_str, sizeof(com1_str), "COM1 %06.3f",
                 (double)(f->com1_freq > 100.0f ? f->com1_freq : 122.800f));
        set_col(r, COL_GREEN);
        draw_text_left(r, x0, y, com1_str, 0.55f);
        y += (int)((float)line_h * 0.75f);
    }

    /* COM2 */
    {
        char com2_str[20];
        snprintf(com2_str, sizeof(com2_str), "COM2 %06.3f",
                 (double)(f->com2_freq > 100.0f ? f->com2_freq : 121.500f));
        set_col(r, COL_GREEN);
        draw_text_left(r, x0, y, com2_str, 0.55f);
    }

    /* Right-side NAV data panel */
    {
        int rx = rect->x + rect->w * 86 / 100;
        int ry = rect->y + 48;   /* Start below the 38px top bar */

        /* NAV1 */
        {
            char nav1_str[32];
            snprintf(nav1_str, sizeof(nav1_str), "NAV1 %06.2f",
                     (double)(f->nav1_freq > 100.0f ? f->nav1_freq : 110.90f));
            set_col(r, COL_CYAN);
            font_draw_scaled_aligned(r, rx, ry, nav1_str, 0.55f, FONT_REGULAR, FONT_ALIGN_RIGHT);
            ry += line_h;
        }

        /* NAV2 */
        {
            char nav2_str[32];
            snprintf(nav2_str, sizeof(nav2_str), "NAV2 %06.2f",
                     (double)(f->nav2_freq > 100.0f ? f->nav2_freq : 113.70f));
            set_col(r, COL_CYAN);
            font_draw_scaled_aligned(r, rx, ry, nav2_str, 0.55f, FONT_REGULAR, FONT_ALIGN_RIGHT);
            ry += line_h;
        }

        /* DME */
        {
            char dme_str[32];
            snprintf(dme_str, sizeof(dme_str), "DME %.1f NM",
                     (double)(f->dme_dist_nm > 0.0f ? f->dme_dist_nm : 0.0f));
            set_col(r, COL_WHITE);
            font_draw_scaled_aligned(r, rx, ry, dme_str, 0.55f, FONT_REGULAR, FONT_ALIGN_RIGHT);
        }
    }

    /* Range indicator (bottom-left) */
    y = rect->y + rect->h - 22;
    set_col(r, COL_CYAN);
    char rng_str[16];
    snprintf(rng_str, sizeof(rng_str), "RNG %d NM", nd->range_nm);
    draw_text_left(r, x0, y, rng_str, 0.6f);

    /* XPDR (bottom-left, next to RNG) */
    {
        const char* mode_str[] = { "OFF", "STBY", "ON", "ALT" };
        int mode = (f->xpdr_mode >= 0 && f->xpdr_mode <= 3) ? f->xpdr_mode : 3;
        char xpdr_str[20];
        snprintf(xpdr_str, sizeof(xpdr_str), "XPDR %04d %s",
                 (f->xpdr_code > 0 ? f->xpdr_code : 1200), mode_str[mode]);
        set_col(r, COL_GREEN);
        draw_text_left(r, x0 + 100, y, xpdr_str, 0.5f);
    }
}

/* =========================================================================
 *  vtable callbacks
 * ========================================================================= */

static void nd_on_init(Instrument* self, App* app)
{
    NDData* nd = (NDData*)self->private_data;
    nd->fmc = app->fmc_state;
    nd->mode = 0;       /* ROSE */
    nd->range_nm = 80;  /* Default range */
    LOG_DEBUG("ND initialized: range=%d NM", nd->range_nm);
}

static void nd_on_update(Instrument* self, const FlightData* fd, float dt)
{
    (void)dt;
    NDData* nd = (NDData*)self->private_data;
    if (!fd) return;

    /* Take full snapshot for render */
    flight_data_snapshot((FlightData*)fd, &nd->fd);
    nd->smooth_hdg = exp_smooth_angle(nd->smooth_hdg, nd->fd.heading_true_deg, 0.12f);

    /* --- Compute ground track from GPS position delta --- */
    if (fabs((double)nd->prev_lat) > 0.001 || fabs((double)nd->prev_lon) > 0.001) {
        GeoPos prev = { nd->prev_lat, nd->prev_lon };
        GeoPos curr = { nd->fd.lat_deg, nd->fd.lon_deg };
        double dist = geo_distance_nm(prev, curr);
        if (dist > 0.005) {  /* Moved at least ~30 ft — track is meaningful */
            nd->track_true_deg = (float)geo_bearing_deg(prev, curr);
            /* Estimate magnetic variation from current heading difference */
            float mag_var = nd->fd.heading_true_deg - nd->fd.heading_mag_deg;
            nd->track_mag_deg = nd->track_true_deg - mag_var;
            if (nd->track_mag_deg < 0.0f)      nd->track_mag_deg += 360.0f;
            if (nd->track_mag_deg >= 360.0f)   nd->track_mag_deg -= 360.0f;
            nd->track_valid = 1;
        }
    }
    nd->prev_lat = (float)nd->fd.lat_deg;
    nd->prev_lon = (float)nd->fd.lon_deg;
}

static void nd_on_render(Instrument* self, SDL_Renderer* renderer)
{
    NDData* nd = (NDData*)self->private_data;
    const SDL_Rect* rect = &self->rect;

    /* Background */
    set_col(renderer, COL_BG);
    SDL_RenderFillRect(renderer, rect);

    /* Use real flight data with smoothed heading */
    FlightDataValues f = nd->fd;
    f.heading_true_deg = nd->smooth_hdg;

    /* Top bar: TRK|MAG + active waypoint info */
    draw_nd_top_bar(renderer, rect, nd, &f);

    draw_compass_rose(renderer, rect, nd, &f);
    draw_nd_data_panel(renderer, rect, &f, nd);

    /* Border */
    set_col(renderer, COL_GRAY);
    SDL_RenderDrawRect(renderer, rect);
}

static int nd_on_event(Instrument* self, const SDL_Event* ev)
{
    NDData* nd = (NDData*)self->private_data;

    /* Only respond to keyboard when mouse is over this ND panel,
     * so typing airport codes like "ZBAA" in the FMC isn't intercepted. */
    int mouse_over = 0;
    {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        mouse_over = (mx >= self->rect.x && mx < self->rect.x + self->rect.w &&
                      my >= self->rect.y && my < self->rect.y + self->rect.h);
    }

    if (ev->type == SDL_KEYDOWN) {
        switch (ev->key.keysym.sym) {
            case SDLK_PLUS:  case SDLK_EQUALS:
                if (mouse_over && nd->range_nm < 320) nd->range_nm *= 2;
                return mouse_over ? 1 : 0;
            case SDLK_MINUS:
                if (mouse_over && nd->range_nm > 10) nd->range_nm /= 2;
                return mouse_over ? 1 : 0;
            case SDLK_m:
                if (mouse_over) nd->mode = (nd->mode + 1) % 2;
                return mouse_over ? 1 : 0;
            case SDLK_w:
                if (mouse_over) nd->show_waypoints = !nd->show_waypoints;
                return mouse_over ? 1 : 0;
            case SDLK_a:
                if (mouse_over) nd->show_airports = !nd->show_airports;
                return mouse_over ? 1 : 0;
            case SDLK_r:
                if (mouse_over) nd->show_route = !nd->show_route;
                return mouse_over ? 1 : 0;
            default: break;
        }
    }
    return 0;
}

static void nd_on_destroy(Instrument* self)
{
    if (self && self->private_data) {
        free(self->private_data);
        self->private_data = NULL;
    }
}

/* =========================================================================
 *  Constructor
 * ========================================================================= */

Instrument* nd_create(void)
{
    Instrument* inst = calloc(1, sizeof(Instrument));
    if (!inst) return NULL;

    NDData* data = calloc(1, sizeof(NDData));
    if (!data) { free(inst); return NULL; }

    data->mode       = 0;
    data->range_nm   = 80;
    data->smooth_hdg = 0.0f;
    data->fmc        = NULL;
    data->show_waypoints = 1;
    data->show_airports  = 1;
    data->show_route     = 1;

    inst->name         = "ND";
    inst->on_init      = nd_on_init;
    inst->on_update    = nd_on_update;
    inst->on_render    = nd_on_render;
    inst->on_event     = nd_on_event;
    inst->on_destroy   = nd_on_destroy;
    inst->private_data = data;

    return inst;
}
