/**
 * @file    nd.c
 * @brief   Navigation Display implementation.
 *
 * Supported modes:
 *   - ROSE: Full 360° compass rose (default)
 *   - ARC:  120° forward arc (not yet implemented)
 *
 * Range: 10, 20, 40, 80, 160, 320 NM (selectable)
 *
 * New features:
 *   - Grid-based spatial hash for nearby nav entry queries (O(1) lookup)
 *   - Course deviation indicator (CDI) needle
 *   - Navaid (VOR/NDB) display with frequency
 *   - Heading-prioritized nav entry sorting
 */

#include "nd.h"
#include "app.h"
#include "data/flight_data.h"
#include "data/navdata.h"
#include "data/nd_data.h"
#include "utils/math_util.h"
#include "utils/font_manager.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

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

/* NDData is now defined in nd.h */

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
    int y0 = rect->y + 4;

    /* === Center: TRK | MAG (above compass, fixed position) === */
    int cx = rect->x + rect->w * 50 / 100;
    int cy = rect->y + rect->h * 55 / 100;                    /* moved down */
    int radius = (int)((float)(rect->h < rect->w ? rect->h : rect->w) * 0.45f);
    if (radius < 50) radius = 50;

    int top_y = rect->y + rect->h * 5 / 100;  /* fixed, independent of compass */

    float track = nd->track_valid ? nd->track_mag_deg : f->heading_mag_deg;
    /* When GS < 30 kt (near-stationary), track is unreliable — fall back to heading */
    if (f->gs_kts < 30.0f) {
        track = f->heading_mag_deg;
    }

    /* Track value in large white */
    char trk_str[8];
    snprintf(trk_str, sizeof(trk_str), "%03.0f", (double)track);
    set_col(r, COL_WHITE);
    font_draw_scaled_aligned(r, cx, top_y, trk_str, 0.75f, FONT_REGULAR, FONT_ALIGN_CENTER);

    /* "TRK" label in small cyan */
    set_col(r, COL_CYAN);
    font_draw_scaled_aligned(r, cx - 35, top_y + 4, "TRK", 0.50f, FONT_REGULAR, FONT_ALIGN_RIGHT);

    /* "MAG" indicator in green */
    set_col(r, COL_GREEN);
    font_draw_scaled_aligned(r, cx + 35, top_y + 4, "MAG", 0.40f, FONT_REGULAR, FONT_ALIGN_LEFT);

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
            double brg_deg = geo_bearing_deg(ac, wpt->pos);

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

            /* Bearing to waypoint — white */
            char brg_str[16];
            snprintf(brg_str, sizeof(brg_str), "%.0f°", brg_deg);
            set_col(r, COL_WHITE);
            font_draw_scaled_aligned(r, rx, y0 + 14, brg_str,
                                     0.45f, FONT_REGULAR, FONT_ALIGN_RIGHT);

            /* Distance in NM — white */
            char dist_str[32];
            snprintf(dist_str, sizeof(dist_str), "%.0f NM", dist_nm);
            set_col(r, COL_WHITE);
            font_draw_scaled_aligned(r, rx, y0 + 26, dist_str,
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
            font_draw_scaled_aligned(r, rx, y0 + 37, eta_str,
                                     0.45f, FONT_REGULAR, FONT_ALIGN_RIGHT);
        }
    }
}

/* =========================================================================
 *  Course deviation indicator (CDI)
 * ========================================================================= */

/**
 * @brief Draw a course deviation indicator needle.
 *
 * The CDI scale spans ±2 dots (full-scale deflection = ±10° or ±5 NM).
 * Center position = on-course, left/right = deviation.
 */
static void draw_course_marker(SDL_Renderer* r, int cx, int cy, int radius,
                               float heading_deg, float course_deg, float deviation)
{
    if (fabsf(deviation) > 1.0f) return;  /* No valid signal */

    /* CDI scale: ±5 dots, each dot = 2° deflection */
    float cdi_angle = DEG2RAD(course_deg - (double)heading_deg - 90.0);
    float base_x = (float)radius * 0.55f * cosf((float)cdi_angle);
    float base_y = (float)radius * 0.55f * sinf((float)cdi_angle);

    /* Perpendicular direction to course line */
    float perp_x = -sinf((float)cdi_angle);
    float perp_y =  cosf((float)cdi_angle);

    /* Deviation: deviation value −1..+1 → ±40 pixels */
    float dev_px = deviation * 60.0f;

    int d1x = cx + (int)(base_x + perp_x * (dev_px - 40.0f));
    int d1y = cy + (int)(base_y + perp_y * (dev_px - 40.0f));
    int d5x = cx + (int)(base_x + perp_x * (dev_px + 40.0f));
    int d5y = cy + (int)(base_y + perp_y * (dev_px + 40.0f));

    /* CDI scale bar (white) */
    set_col(r, COL_WHITE);
    SDL_RenderDrawLine(r, d1x, d1y, d5x, d5y);

    /* Center mark (circle) */
    int cmx = cx + (int)(base_x + perp_x * dev_px);
    int cmy = cy + (int)(base_y + perp_y * dev_px);
    draw_filled_circle(r, cmx, cmy, 2);

    /* Dot marks */
    for (int dot = -2; dot <= 2; dot++) {
        if (dot == 0) continue;
        float dot_off = (float)dot * 16.0f;
        int dx = cx + (int)(base_x + perp_x * (dev_px + dot_off));
        int dy = cy + (int)(base_y + perp_y * (dev_px + dot_off));
        draw_filled_circle(r, dx, dy, 1);
    }
}

/* =========================================================================
 *  Navigation entry symbols
 * ========================================================================= */

/** Draw a diamond symbol (waypoint) */
static void draw_diamond(SDL_Renderer* r, int cx, int cy, int size)
{
    SDL_RenderDrawLine(r, cx - size, cy, cx, cy - size);
    SDL_RenderDrawLine(r, cx, cy - size, cx + size, cy);
    SDL_RenderDrawLine(r, cx + size, cy, cx, cy + size);
    SDL_RenderDrawLine(r, cx, cy + size, cx - size, cy);
}

/** Draw a square symbol (airport) */
static void draw_square(SDL_Renderer* r, int cx, int cy, int size)
{
    SDL_Rect sq = { cx - size, cy - size, size * 2, size * 2 };
    SDL_RenderDrawRect(r, &sq);
}

/** Draw a triangle symbol (VOR/NDB/tower) */
static void draw_triangle(SDL_Renderer* r, int cx, int cy, int size)
{
    SDL_RenderDrawLine(r, cx, cy - size, cx - size, cy + size);
    SDL_RenderDrawLine(r, cx - size, cy + size, cx + size, cy + size);
    SDL_RenderDrawLine(r, cx + size, cy + size, cx, cy - size);
}

/* =========================================================================
 *  Mode name helpers
 * ========================================================================= */

static const char* nd_mode_name(int mode)
{
    switch (mode) {
        case 0:  return "VOR";
        case 1:  return "MAP";
        case 2:  return "APP";
        case 3:  return "PLN";
        default: return "???";
    }
}

/* =========================================================================
 *  Shared: draw the route overlay (reused by MAP and PLN modes)
 * ========================================================================= */

static void draw_route_overlay(SDL_Renderer* r, const SDL_Rect* rect, NDData* nd,
                                const FlightDataValues* f, GeoPos ac,
                                float hdg, float px_nm, int cx, int cy,
                                int radius)
{
    (void)rect;

    /* =====================================================================
     *  Navigation entries from spatial hash (heading-prioritized)
     * ===================================================================== */
    int max_display = (nd->range_nm <= 40) ? 80 : (nd->range_nm <= 80) ? 120 : 160;

    if (nd->spatial_hash && (nd->show_waypoints || nd->show_airports || nd->show_navaids)) {
        int count = spatial_hash_query(nd->spatial_hash,
                                       ac.lat_deg, ac.lon_deg,
                                       hdg, (float)nd->range_nm,
                                       nd->nearby_entries, max_display);
        nd->nearby_count = count;

        int waypoint_labels = (nd->range_nm <= 80) ? 1 : 0;

        for (int i = 0; i < count; i++) {
            NavQueryResult* qr = &nd->nearby_entries[i];
            NavSpatialEntry* entry = qr->entry;
            if (!entry) continue;

            /* Use pre-computed dist/bearing from spatial_hash_query —
             * avoids a second Haversine call. */
            float px_dist = qr->dist_nm * px_nm;
            float rad     = (float)DEG2RAD((double)qr->bearing_deg - (double)hdg - 90.0);
            int vis = (qr->dist_nm <= (double)nd->range_nm * 1.1);
            if (px_dist > (float)radius) px_dist = (float)radius - 4.0f;
            int sx = cx + (int)(px_dist * cosf(rad));
            int sy = cy + (int)(px_dist * sinf(rad));

            switch (entry->type) {
            case NAV_WAYPOINT:
                if (!nd->show_waypoints) continue;
                set_col(r, COL_CYAN);
                draw_diamond(r, sx, sy, 3);
                if (vis && waypoint_labels && entry->ident[0]) {
                    set_col(r, COL_CYAN);
                    font_draw_scaled_aligned(r, sx + 5, sy - 4, entry->ident,
                                             0.35f, FONT_REGULAR, FONT_ALIGN_LEFT);
                }
                break;

            case NAV_AIRPORT:
                if (!nd->show_airports) continue;
                set_col(r, COL_MAGENTA);
                draw_square(r, sx, sy, 4);
                if (vis && waypoint_labels && entry->ident[0]) {
                    set_col(r, COL_MAGENTA);
                    font_draw_scaled_aligned(r, sx + 6, sy - 4, entry->ident,
                                             0.38f, FONT_REGULAR, FONT_ALIGN_LEFT);
                }
                break;

            case NAV_VOR:
            case NAV_NDB:
            case NAV_TOWER:
                if (!nd->show_navaids) continue;
                set_col(r, (entry->type == NAV_VOR) ? COL_GREEN : COL_AMBER);
                draw_triangle(r, sx, sy, 4);
                if (vis && waypoint_labels && entry->ident[0]) {
                    set_col(r, (entry->type == NAV_VOR) ? COL_GREEN : COL_AMBER);
                    char navaid_label[32];
                    if (entry->freq_khz > 0.0f) {
                        snprintf(navaid_label, sizeof(navaid_label), "%s %.2f",
                                 entry->ident, (double)(entry->freq_khz / 100.0f));
                    } else {
                        snprintf(navaid_label, sizeof(navaid_label), "%s", entry->ident);
                    }
                    font_draw_scaled_aligned(r, sx + 6, sy - 4, navaid_label,
                                             0.35f, FONT_REGULAR, FONT_ALIGN_LEFT);
                }
                break;

            default:
                break;
            }
        }
    }

    /* =====================================================================
     *  Active flight plan route
     * ===================================================================== */
    if (nd->show_route && nd->fmc) {
        FlightPlan* fp = &nd->fmc->flight_plan;
        if (fp->waypoint_count >= 2) {
            for (int i = 0; i < fp->waypoint_count - 1; i++) {
                int x1, y1, x2, y2;
                geo_to_screen(ac, hdg, px_nm, cx, cy,
                              fp->waypoints[i].pos,
                              &x1, &y1, (float)nd->range_nm, radius);
                geo_to_screen(ac, hdg, px_nm, cx, cy,
                              fp->waypoints[i + 1].pos,
                              &x2, &y2, (float)nd->range_nm, radius);
                int is_active = (i == fp->active_waypoint_index - 1);
                if (is_active) set_col(r, COL_MAGENTA);
                else           set_col(r, COL_GREEN);
                int lw = is_active ? 3 : 2;
                draw_thick_line(r, x1, y1, x2, y2, lw);

                if (nd->range_nm <= 80) {
                    double leg_brg = geo_bearing_deg(fp->waypoints[i].pos,
                                                     fp->waypoints[i + 1].pos);
                    int mid_x = (x1 + x2) / 2;
                    int mid_y = (y1 + y2) / 2;
                    char crs_label[8];
                    snprintf(crs_label, sizeof(crs_label), "%.0f°", leg_brg);
                    set_col(r, COL_CYAN);
                    font_draw_scaled_aligned(r, mid_x + 8, mid_y - 6, crs_label,
                                             0.35f, FONT_REGULAR, FONT_ALIGN_LEFT);
                }
            }
        }
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
 *  MODE 0: VOR — full compass rose + VOR radial + CDI
 * ========================================================================= */

static void draw_vor_mode(SDL_Renderer* r, const SDL_Rect* rect, NDData* nd,
                          const FlightDataValues* f)
{
    int cx   = rect->x + rect->w * 50 / 100;
    int cy   = rect->y + rect->h * 55 / 100;
    int radius = (int)((float)(rect->h < rect->w ? rect->h : rect->w) * 0.45f);
    if (radius < 50) radius = 50;

    float hdg = f->heading_mag_deg;
    float px_nm = (float)radius / (float)nd->range_nm;

    /* --- VOR frequency & identifier display (top-left) --- */
    {
        set_col(r, COL_CYAN);
        char vor_info[32];
        snprintf(vor_info, sizeof(vor_info), "VOR1 %06.2f", (double)f->nav1_freq);
        font_draw_scaled_aligned(r, rect->x + 20, rect->y + 20, vor_info,
                                 0.55f, FONT_BOLD, FONT_ALIGN_LEFT);
    }

    /* --- Range rings --- */
    int rings[] = { nd->range_nm / 4, nd->range_nm / 2, nd->range_nm * 3 / 4, nd->range_nm };
    for (int ri = 0; ri < 4; ri++) {
        int r_px = (int)((float)rings[ri] * px_nm);
        if (r_px > radius) r_px = radius;
        set_col(r, COL_GRAY);
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
    }

    /* --- Compass ticks & labels --- */
    for (int deg = 0; deg < 360; deg += 5) {
        float angle_rad = (float)DEG2RAD((double)deg - (double)hdg - 90.0);
        float c = cosf(angle_rad), s = sinf(angle_rad);
        float outer = (float)radius, inner;
        int is_cardinal = (deg % 90 == 0);
        int is_10 = (deg % 10 == 0);
        if (is_cardinal) inner = outer - 22.0f;
        else if (is_10)  inner = outer - 16.0f;
        else             inner = outer - 10.0f;
        set_col(r, COL_WHITE);
        SDL_RenderDrawLine(r, cx + (int)(inner * c), cy + (int)(inner * s),
                           cx + (int)(outer * c), cy + (int)(outer * s));
        if (is_cardinal) {
            const char* lbl = (deg == 0) ? "N" : (deg == 90) ? "E"
                            : (deg == 180) ? "S" : "W";
            float lx = (outer + 18.0f) * c, ly = (outer + 18.0f) * s;
            set_col(r, COL_CYAN);
            draw_text_simple(r, cx + (int)lx, cy + (int)ly, lbl, 0.55f);
        }
    }

    /* --- VOR course selector + CDI --- */
    if (f->nav1_freq > 100.0f) {
        float crs = f->nav1_course;
        float cdi = f->nav1_cdi;

        /* TO/FROM indicator arrow */
        float crs_rad = (float)DEG2RAD((double)crs - (double)hdg - 90.0);
        float crs_x = (float)radius * 0.85f * cosf(crs_rad);
        float crs_y = (float)radius * 0.85f * sinf(crs_rad);

        set_col(r, COL_GREEN);
        /* Small arrow at course indicator position */
        SDL_RenderDrawLine(r, cx + (int)crs_x - 6, cy + (int)crs_y - 4,
                           cx + (int)crs_x,     cy + (int)crs_y);
        SDL_RenderDrawLine(r, cx + (int)crs_x + 6, cy + (int)crs_y - 4,
                           cx + (int)crs_x,     cy + (int)crs_y);

        /* CDI needle (rotating with VOR course) */
        float needle_rad = (float)DEG2RAD((double)crs - (double)hdg - 90.0);
        float nd_base_x = (float)radius * 0.18f * cosf(needle_rad);
        float nd_base_y = (float)radius * 0.18f * sinf(needle_rad);
        float nd_tip_x  = (float)radius * 0.85f * cosf(needle_rad);
        float nd_tip_y  = (float)radius * 0.85f * sinf(needle_rad);
        set_col(r, COL_GREEN);
        SDL_RenderDrawLine(r, cx + (int)nd_base_x, cy + (int)nd_base_y,
                           cx + (int)nd_tip_x, cy + (int)nd_tip_y);

        /* CDI deviation dots perpendicular to course */
        float perp_x = -sinf(needle_rad), perp_y = cosf(needle_rad);
        float dev_px = cdi * 50.0f;  /* ±50px full scale */
        for (int dot = -2; dot <= 2; dot++) {
            float dot_off = (float)dot * 16.0f + dev_px;
            int dx = cx + (int)((float)radius * 0.55f * cosf(needle_rad) + perp_x * dot_off);
            int dy = cy + (int)((float)radius * 0.55f * sinf(needle_rad) + perp_y * dot_off);
            set_col(r, (dot == 0) ? COL_WHITE : COL_GRAY);
            draw_filled_circle(r, dx, dy, (dot == 0) ? 2 : 1);
        }

        /* Course readout (bottom) */
        char crs_str[16];
        snprintf(crs_str, sizeof(crs_str), "CRS %.0f°", (double)crs);
        set_col(r, COL_GREEN);
        font_draw_scaled_aligned(r, cx, cy + radius + 8, crs_str,
                                 0.50f, FONT_REGULAR, FONT_ALIGN_CENTER);
    }

    /* --- DME distance --- */
    if (f->dme_dist_nm > 0.0f) {
        char dme_str[24];
        snprintf(dme_str, sizeof(dme_str), "DME %.1f NM", (double)f->dme_dist_nm);
        set_col(r, COL_GREEN);
        font_draw_scaled_aligned(r, rect->x + 20, rect->y + rect->h - 40,
                                 dme_str, 0.55f, FONT_BOLD, FONT_ALIGN_LEFT);
    }

    /* --- Heading bug --- */
    float ap_hdg_rad = (float)DEG2RAD((double)f->ap_hdg - (double)hdg - 90.0);
    float bug_x = (float)radius * cosf(ap_hdg_rad);
    float bug_y = (float)radius * sinf(ap_hdg_rad);
    set_col(r, COL_CYAN);
    SDL_RenderDrawLine(r, cx + (int)bug_x, cy + (int)bug_y - 5,
                       cx + (int)bug_x + 5, cy + (int)bug_y);
    SDL_RenderDrawLine(r, cx + (int)bug_x + 5, cy + (int)bug_y,
                       cx + (int)bug_x, cy + (int)bug_y + 5);
    SDL_RenderDrawLine(r, cx + (int)bug_x, cy + (int)bug_y + 5,
                       cx + (int)bug_x - 5, cy + (int)bug_y);
    SDL_RenderDrawLine(r, cx + (int)bug_x - 5, cy + (int)bug_y,
                       cx + (int)bug_x, cy + (int)bug_y - 5);

    /* --- Aircraft symbol --- */
    set_col(r, COL_WHITE);
    SDL_RenderDrawLine(r, cx, cy - 12, cx, cy + 12);
    SDL_RenderDrawLine(r, cx - 15, cy - 3, cx + 15, cy - 3);
    SDL_RenderDrawLine(r, cx - 5, cy + 6, cx + 5, cy + 6);
    draw_filled_circle(r, cx, cy, 3);

    /* --- Lubber line --- */
    set_col(r, COL_WHITE);
    SDL_RenderDrawLine(r, cx, cy - radius, cx - 8, cy - radius - 12);
    SDL_RenderDrawLine(r, cx, cy - radius, cx + 8, cy - radius - 12);
    SDL_RenderDrawLine(r, cx - 8, cy - radius - 12, cx + 8, cy - radius - 12);

    /* --- Track line --- */
    if (f->gs_kts > 30.0f) {
        float trk_rad = (float)DEG2RAD(-90.0);
        set_col(r, COL_GREEN);
        SDL_RenderDrawLine(r, cx, cy, cx + (int)((float)radius * cosf(trk_rad)),
                           cy + (int)((float)radius * sinf(trk_rad)));
    }

    /* --- Wind vector --- */
    if (f->wind_speed_kts > 2.0f) {
        float wind_rad = (float)DEG2RAD((double)f->wind_dir_deg - (double)hdg - 90.0);
        int wx = (int)((float)cx + (float)radius * 0.45f * cosf(wind_rad));
        int wy = (int)((float)cy + (float)radius * 0.45f * sinf(wind_rad));
        int wl = (int)(f->wind_speed_kts * 2.0f);
        float arrow_rad = (float)DEG2RAD((double)f->wind_dir_deg + 180.0 - (double)hdg - 90.0);
        set_col(r, COL_WHITE);
        SDL_RenderDrawLine(r, wx, wy,
                           wx + (int)((float)wl * cosf(arrow_rad)),
                           wy + (int)((float)wl * sinf(arrow_rad)));
        char wspd[8];
        snprintf(wspd, sizeof(wspd), "%.0f", (double)f->wind_speed_kts);
        draw_text_simple(r, wx + (int)((float)wl * cosf(arrow_rad)) + 6,
                         wy + (int)((float)wl * sinf(arrow_rad)) - 6, wspd, 0.35f);
    }
}

/* =========================================================================
 *  MODE 1: MAP — compass rose + full navigation overlay
 * ========================================================================= */

static void draw_map_mode(SDL_Renderer* r, const SDL_Rect* rect, NDData* nd,
                          const FlightDataValues* f)
{
    int cx   = rect->x + rect->w * 50 / 100;  /* Centered horizontally */
    int cy   = rect->y + rect->h * 55 / 100;
    int radius = (int)((float)(rect->h < rect->w ? rect->h : rect->w) * 0.45f);
    if (radius < 50) radius = 50;

    float hdg = f->heading_mag_deg;

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
    SDL_RenderDrawLine(r, cx, cy - 12, cx, cy + 12);
    SDL_RenderDrawLine(r, cx - 15, cy - 3, cx + 15, cy - 3);
    SDL_RenderDrawLine(r, cx - 5, cy + 6, cx + 5, cy + 6);
    draw_filled_circle(r, cx, cy, 3);

    /* --- Top Heading Indicator (Fixed lubber line/triangle) --- */
    set_col(r, COL_WHITE);
    SDL_RenderDrawLine(r, cx, cy - radius, cx - 8, cy - radius - 12);
    SDL_RenderDrawLine(r, cx, cy - radius, cx + 8, cy - radius - 12);
    SDL_RenderDrawLine(r, cx - 8, cy - radius - 12, cx + 8, cy - radius - 12);

    /* --- Track line (if moving) --- */
    if (f->gs_kts > 30.0f) {
        float trk_rad = (float)DEG2RAD(-90.0);
        set_col(r, COL_GREEN);
        SDL_RenderDrawLine(r, cx, cy, cx + (int)((float)radius * cosf(trk_rad)),
                           cy + (int)((float)radius * sinf(trk_rad)));
    }

    /* --- Wind vector --- */
    if (f->wind_speed_kts > 2.0f) {
        float wind_scale = 2.0f;
        float wind_rad = (float)DEG2RAD((double)f->wind_dir_deg - (double)hdg - 90.0);
        int wx = (int)((float)cx + (float)radius * 0.5f * cosf(wind_rad));
        int wy = (int)((float)cy + (float)radius * 0.5f * sinf(wind_rad));
        int wl = (int)(f->wind_speed_kts * wind_scale);
        float arrow_rad = (float)DEG2RAD((double)f->wind_dir_deg + 180.0 - (double)hdg - 90.0);
        set_col(r, COL_WHITE);
        SDL_RenderDrawLine(r, wx, wy,
                           wx + (int)((float)wl * cosf(arrow_rad)),
                           wy + (int)((float)wl * sinf(arrow_rad)));
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

        char wspd[8];
        snprintf(wspd, sizeof(wspd), "%.0f", (double)f->wind_speed_kts);
        set_col(r, COL_WHITE);
        draw_text_simple(r, ax + 6, ay - 8, wspd, 0.35f);
    }

    /* --- Aircraft position --- */
    GeoPos ac;
    ac.lat_deg = f->lat_deg;
    ac.lon_deg = f->lon_deg;
    if (fabs(ac.lat_deg) < 0.01 && fabs(ac.lon_deg) < 0.01) {
        ac.lat_deg = 39.0;
        ac.lon_deg = 117.0;
    }

    float px_nm = (float)radius / (float)nd->range_nm;

    /* Route + nav overlay (shared) */
    draw_route_overlay(r, rect, nd, f, ac, hdg, px_nm, cx, cy, radius);

    /* CDI */
    if (nd->show_course && f->nav1_freq > 100.0f) {
        float crs = (f->nav1_course > 0.0f) ? f->nav1_course : nd->crs_selector_deg;
        draw_course_marker(r, cx, cy, radius, hdg, crs, f->nav1_cdi);
    }
}

/* =========================================================================
 *  MODE 2: APP — ILS approach display (localizer + glideslope)
 * ========================================================================= */

static void draw_app_mode(SDL_Renderer* r, const SDL_Rect* rect, NDData* nd,
                          const FlightDataValues* f)
{
    int cx   = rect->x + rect->w / 2;
    int cy   = rect->y + rect->h / 2;
    int radius = (int)((float)(rect->h < rect->w ? rect->h : rect->w) * 0.42f);
    if (radius < 50) radius = 50;

    float hdg = f->heading_mag_deg;

    /* --- Approach reference (ILS course) --- */
    float app_crs = f->nav1_course;

    /* --- Localizer scale (top half) --- */
    {
        int lx = cx;
        int ly = cy - radius + 40;
        int lw = radius * 2 - 80;

        /* Labels */
        set_col(r, COL_CYAN);
        font_draw_scaled_aligned(r, cx, rect->y + 18, "APP", 0.55f, FONT_BOLD, FONT_ALIGN_CENTER);

        char ils_info[32];
        snprintf(ils_info, sizeof(ils_info), "ILS %.2f", (double)f->nav1_freq);
        font_draw_scaled_aligned(r, cx, rect->y + 35, ils_info,
                                 0.45f, FONT_REGULAR, FONT_ALIGN_CENTER);

        /* LOC scale line */
        set_col(r, COL_WHITE);
        SDL_RenderDrawLine(r, lx, ly, lx + lw, ly);

        /* Center mark */
        set_col(r, COL_AMBER);
        SDL_RenderDrawLine(r, cx, ly - 8, cx, ly + 8);

        /* Dot marks (±2 dots) */
        for (int dot = -2; dot <= 2; dot++) {
            if (dot == 0) continue;
            int dx = cx + dot * (lw / 10);
            set_col(r, COL_WHITE);
            SDL_RenderDrawLine(r, dx, ly - 4, dx, ly + 4);
        }

        /* LOC deviation diamond */
        float loc_dev = f->nav1_cdi;  /* −1..+1 */
        int dev_x = cx + (int)(loc_dev * (float)(lw / 2) * 0.8f);
        set_col(r, COL_MAGENTA);
        draw_diamond(r, dev_x, ly, 5);
    }

    /* --- Glideslope scale (right side) --- */
    {
        int gx = cx + radius - 30;
        int gy = cy - radius + 60;
        int gh = radius * 2 - 120;

        /* GS scale line */
        set_col(r, COL_WHITE);
        SDL_RenderDrawLine(r, gx, gy, gx, gy + gh);

        /* Center mark */
        set_col(r, COL_AMBER);
        SDL_RenderDrawLine(r, gx - 8, cy, gx + 8, cy);

        /* Dot marks */
        for (int dot = -2; dot <= 2; dot++) {
            if (dot == 0) continue;
            int dy = cy + dot * (gh / 10);
            set_col(r, COL_WHITE);
            SDL_RenderDrawLine(r, gx - 4, dy, gx + 4, dy);
        }

        /* GS deviation diamond (NAV2 CDI or inverted NAV1 vertical) */
        float gs_dev = f->nav2_cdi;
        int dev_y = cy + (int)(gs_dev * (float)(gh / 2) * 0.8f);
        set_col(r, COL_MAGENTA);
        draw_diamond(r, gx, dev_y, 5);

        /* GS label */
        set_col(r, COL_GREEN);
        draw_text_simple(r, gx + 15, gy - 10, "GS", 0.40f);
    }

    /* --- Heading scale at bottom --- */
    {
        int hx = cx;
        int hy = cy + radius - 25;
        int hw = radius * 2 - 60;

        set_col(r, COL_WHITE);
        SDL_RenderDrawLine(r, hx, hy, hx + hw, hy);

        /* Heading ticks every 5° */
        for (int d = -30; d <= 30; d += 5) {
            float deg = hdg + (float)d;
            if (deg < 0.0f) deg += 360.0f;
            if (deg >= 360.0f) deg -= 360.0f;

            int tx = cx + d * (hw / 60);
            int tick_h = (d % 10 == 0) ? 8 : 4;
            set_col(r, COL_WHITE);
            SDL_RenderDrawLine(r, tx, hy, tx, hy + tick_h);

            if (d % 10 == 0) {
                char hdg_label[4];
                snprintf(hdg_label, sizeof(hdg_label), "%.0f", (double)deg);
                set_col(r, COL_CYAN);
                font_draw_scaled_aligned(r, tx, hy + 14, hdg_label,
                                         0.35f, FONT_REGULAR, FONT_ALIGN_CENTER);
            }
        }

        /* Center triangle (lubber line) */
        set_col(r, COL_WHITE);
        SDL_RenderDrawLine(r, cx, hy - 6, cx - 5, hy);
        SDL_RenderDrawLine(r, cx, hy - 6, cx + 5, hy);
    }

    /* --- DME distance readout --- */
    if (f->dme_dist_nm > 0.0f) {
        char dme_str[24];
        snprintf(dme_str, sizeof(dme_str), "DME %.1f", (double)f->dme_dist_nm);
        set_col(r, COL_GREEN);
        font_draw_scaled_aligned(r, rect->x + rect->w - 50, rect->y + rect->h - 30,
                                 dme_str, 0.50f, FONT_BOLD, FONT_ALIGN_RIGHT);
    }

    /* --- Ground speed --- */
    {
        char gs_str[16];
        snprintf(gs_str, sizeof(gs_str), "GS %.0f", (double)f->gs_kts);
        set_col(r, COL_WHITE);
        font_draw_scaled_aligned(r, rect->x + 30, rect->y + rect->h - 30,
                                 gs_str, 0.50f, FONT_BOLD, FONT_ALIGN_LEFT);
    }

    (void)nd;
}

/* =========================================================================
 *  MODE 3: PLN — static north-up plan view for route review
 * ========================================================================= */

static void draw_pln_mode(SDL_Renderer* r, const SDL_Rect* rect, NDData* nd,
                          const FlightDataValues* f)
{
    int cx   = rect->x + rect->w / 2;
    int cy   = rect->y + rect->h / 2;
    int radius = (int)((float)(rect->h < rect->w ? rect->h : rect->w) * 0.42f);
    if (radius < 50) radius = 50;

    /* PLAN mode: north-up, no heading rotation */
    float hdg = 0.0f;

    /* Aircraft position */
    GeoPos ac;
    ac.lat_deg = f->lat_deg;
    ac.lon_deg = f->lon_deg;
    if (fabs(ac.lat_deg) < 0.01 && fabs(ac.lon_deg) < 0.01) {
        ac.lat_deg = 39.0;
        ac.lon_deg = 117.0;
    }

    float px_nm = (float)radius / (float)nd->range_nm;

    /* --- "PLAN" indicator --- */
    set_col(r, COL_CYAN);
    font_draw_scaled_aligned(r, rect->x + rect->w / 2, rect->y + 16, "PLAN",
                             0.60f, FONT_BOLD, FONT_ALIGN_CENTER);

    /* --- North-up compass rose (static) --- */
    /* Range rings */
    int rings[] = { nd->range_nm / 4, nd->range_nm / 2, nd->range_nm * 3 / 4, nd->range_nm };
    for (int ri = 0; ri < 4; ri++) {
        int r_px = (int)((float)rings[ri] * px_nm);
        if (r_px > radius) r_px = radius;
        set_col(r, COL_GRAY);
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
    }

    /* Static compass ticks (north-up, 0° = up) */
    for (int deg = 0; deg < 360; deg += 5) {
        float angle_rad = (float)DEG2RAD((double)deg - 90.0);  /* 0° = top (north) */
        float c = cosf(angle_rad), s = sinf(angle_rad);
        float outer = (float)radius, inner;
        int is_cardinal = (deg % 90 == 0);
        int is_10 = (deg % 10 == 0);
        if (is_cardinal) inner = outer - 20.0f;
        else if (is_10)  inner = outer - 14.0f;
        else             inner = outer - 8.0f;
        set_col(r, COL_GRAY);
        SDL_RenderDrawLine(r, cx + (int)(inner * c), cy + (int)(inner * s),
                           cx + (int)(outer * c), cy + (int)(outer * s));
        if (is_cardinal) {
            const char* lbl = (deg == 0) ? "N" : (deg == 90) ? "E"
                            : (deg == 180) ? "S" : "W";
            float lx = (outer + 16.0f) * c, ly = (outer + 16.0f) * s;
            set_col(r, COL_CYAN);
            draw_text_simple(r, cx + (int)lx, cy + (int)ly, lbl, 0.50f);
        }
    }

    /* --- Aircraft position symbol --- */
    set_col(r, COL_WHITE);
    draw_filled_circle(r, cx, cy, 4);
    /* Crosshair */
    SDL_RenderDrawLine(r, cx - 10, cy, cx + 10, cy);
    SDL_RenderDrawLine(r, cx, cy - 10, cx, cy + 10);

    /* --- Route overlay --- */
    draw_route_overlay(r, rect, nd, f, ac, hdg, px_nm, cx, cy, radius);

    /* --- Active waypoint info at bottom --- */
    if (nd->fmc && nd->fmc->flight_plan.waypoint_count > 0) {
        FlightPlan* fp = &nd->fmc->flight_plan;
        char plan_info[64];
        snprintf(plan_info, sizeof(plan_info), "%s → %s  %d WPT  %.0f NM",
                 fp->departure.icao[0] ? fp->departure.icao : "????",
                 fp->arrival.icao[0]   ? fp->arrival.icao   : "????",
                 fp->waypoint_count, (double)fp->total_distance_nm);
        set_col(r, COL_CYAN);
        font_draw_scaled_aligned(r, rect->x + rect->w / 2,
                                 rect->y + rect->h - 18, plan_info,
                                 0.40f, FONT_REGULAR, FONT_ALIGN_CENTER);
    }
}

/* =========================================================================
 *  Left data panel
 * ========================================================================= */

static void draw_nd_data_panel(SDL_Renderer* r, const SDL_Rect* rect,
                               const FlightDataValues* f, NDData* nd)
{
    int x0 = rect->x + 15;
    int y  = rect->y + 55;

    /* ==== Highlighted Data: GS, TAS, WIND ==== */

    /* Ground speed */
    set_col(r, COL_WHITE);
    draw_text_left(r, x0, y, "GS", 0.60f);
    char gs_str[16];
    snprintf(gs_str, sizeof(gs_str), "%.0f", (double)f->gs_kts);
    set_col(r, COL_GREEN);
    draw_text_left(r, x0 + 45, y - 2, gs_str, 0.80f);
    y += 28;

    /* True airspeed */
    set_col(r, COL_WHITE);
    draw_text_left(r, x0, y, "TAS", 0.60f);
    char tas_str[16];
    snprintf(tas_str, sizeof(tas_str), "%.0f", (double)f->tas_kts);
    set_col(r, COL_GREEN);
    draw_text_left(r, x0 + 45, y - 2, tas_str, 0.80f);
    y += 32;

    /* Wind Data */
    char wind_str[32];
    snprintf(wind_str, sizeof(wind_str), "%03.0f / %02.0f KT",
             (double)f->wind_dir_deg, (double)f->wind_speed_kts);
    set_col(r, COL_CYAN);
    draw_text_left(r, x0, y, wind_str, 0.65f);
    y += 24;

    /* OAT (smaller) */
    set_col(r, COL_WHITE);
    char oat_str[16];
    snprintf(oat_str, sizeof(oat_str), "OAT %+.0f C", (double)f->oat_c);
    draw_text_left(r, x0, y, oat_str, 0.50f);

    /* ==== Right-side NAV data panel ==== */
    {
        int rx = rect->x + rect->w - 15;
        int ry = rect->y + 55;

        int nav_line_h = 22;

        /* NAV1 */
        char nav1_str[32];
        snprintf(nav1_str, sizeof(nav1_str), "VOR1  %06.2f",
                 (double)(f->nav1_freq > 100.0f ? f->nav1_freq : 110.90f));
        set_col(r, COL_CYAN);
        font_draw_scaled_aligned(r, rx, ry, nav1_str, 0.50f, FONT_REGULAR, FONT_ALIGN_RIGHT);
        ry += nav_line_h;

        /* NAV2 */
        char nav2_str[32];
        snprintf(nav2_str, sizeof(nav2_str), "VOR2  %06.2f",
                 (double)(f->nav2_freq > 100.0f ? f->nav2_freq : 113.70f));
        set_col(r, COL_CYAN);
        font_draw_scaled_aligned(r, rx, ry, nav2_str, 0.50f, FONT_REGULAR, FONT_ALIGN_RIGHT);
        ry += nav_line_h;

        /* DME */
        char dme_str[32];
        snprintf(dme_str, sizeof(dme_str), "DME  %.1f NM",
                 (double)(f->dme_dist_nm > 0.0f ? f->dme_dist_nm : 0.0f));
        set_col(r, COL_GREEN);
        font_draw_scaled_aligned(r, rx, ry, dme_str, 0.50f, FONT_REGULAR, FONT_ALIGN_RIGHT);

        /* Nearby count from spatial hash */
        if (nd->spatial_hash && nd->nearby_count > 0) {
            ry += nav_line_h;
            char cnt_str[24];
            snprintf(cnt_str, sizeof(cnt_str), "NAV %d", nd->nearby_count);
            set_col(r, COL_CYAN);
            font_draw_scaled_aligned(r, rx, ry, cnt_str, 0.45f, FONT_REGULAR, FONT_ALIGN_RIGHT);
        }
    }

    /* ==== Bottom Information ==== */
    int by = rect->y + rect->h - 25;
    set_col(r, COL_CYAN);
    char rng_str[16];
    snprintf(rng_str, sizeof(rng_str), "RNG %d NM", nd->range_nm);
    draw_text_left(r, x0, by, rng_str, 0.6f);

    /* XPDR (bottom-right) */
    {
        const char* mode_str[] = { "OFF", "STBY", "ON", "ALT" };
        int mode = (f->xpdr_mode >= 0 && f->xpdr_mode <= 3) ? f->xpdr_mode : 3;
        char xpdr_str[20];
        snprintf(xpdr_str, sizeof(xpdr_str), "XPDR %04d %s",
                 (f->xpdr_code > 0 ? f->xpdr_code : 1200), mode_str[mode]);
        set_col(r, COL_GREEN);
        font_draw_scaled_aligned(r, rect->x + rect->w - 15, by, xpdr_str, 0.55f, FONT_REGULAR, FONT_ALIGN_RIGHT);
    }
}

/* =========================================================================
 *  vtable callbacks
 * ========================================================================= */

static void nd_on_init(Instrument* self, App* app)
{
    NDData* nd = (NDData*)self->private_data;
    nd->fmc           = app->fmc_state;
    nd->mode          = 1;       /* Default: MAP */
    nd->range_nm      = 80;      /* Default range */
    nd->spatial_hash  = NULL;
    nd->show_course   = 1;
    nd->show_navaids  = 1;

    /* Get spatial hash from FMCState */
    if (nd->fmc && nd->fmc->spatial_hash) {
        nd->spatial_hash = nd->fmc->spatial_hash;
        LOG_INFO("ND: spatial hash connected (%d entries)",
                 nd->spatial_hash->total_entries);
    }

    LOG_DEBUG("ND initialized: range=%d NM, spatial_hash=%s",
              nd->range_nm, nd->spatial_hash ? "OK" : "NONE");
}

static void nd_on_update(Instrument* self, const FlightData* fd, float dt)
{
    (void)dt;
    NDData* nd = (NDData*)self->private_data;
    if (!fd) return;

    /* Take full snapshot for render */
    flight_data_snapshot((FlightData*)fd, &nd->fd);

    /* --- Override with high-accuracy RREF values (named paths, no index ambiguity) --- */
    if (nd->fd.dref_nd_valid & 0x04) {
        nd->fd.heading_mag_deg = nd->fd.dref_nd_mag_psi;   /* MAG — RREF, degrees */
    }
    if (nd->fd.dref_nd_valid & 0x08) {
        nd->fd.tas_kts = nd->fd.dref_nd_true_airspeed * 1.94384f; /* TAS m/s→kts */
    }
    if (nd->fd.dref_nd_valid & 0x10) {
        nd->fd.gs_kts = nd->fd.dref_nd_groundspeed * 1.94384f;   /* GS  m/s→kts */
    }

    nd->smooth_hdg = exp_smooth_angle(nd->smooth_hdg, nd->fd.heading_mag_deg, 0.12f);

    /* --- Ground track from X-Plane RREF (mag_hpath = magnetic track °) --- */
    if (nd->fd.dref_nd_valid & 0x40) {
        nd->track_mag_deg  = nd->fd.dref_nd_mag_hpath;          /* already magnetic */
        nd->track_true_deg = nd->fd.dref_nd_hpath;              /* true track */
        nd->track_valid = 1;
    }

    /* Update course data from NAV1 */
    nd->crs_selector_deg = nd->fd.nav1_course;
    nd->crs_deviation    = nd->fd.nav1_cdi;
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
    f.heading_mag_deg  = nd->smooth_hdg;

    /* Top bar: TRK|MAG + active waypoint info */
    draw_nd_top_bar(renderer, rect, nd, &f);

    /* Dispatch to current mode */
    switch (nd->mode) {
        case 0: draw_vor_mode(renderer, rect, nd, &f); break;
        case 1: draw_map_mode(renderer, rect, nd, &f); break;
        case 2: draw_app_mode(renderer, rect, nd, &f); break;
        case 3: draw_pln_mode(renderer, rect, nd, &f); break;
        default: draw_map_mode(renderer, rect, nd, &f); break;
    }

    /* Mode indicator (bottom-center) */
    {
        char mode_str[16];
        snprintf(mode_str, sizeof(mode_str), "%s", nd_mode_name(nd->mode));
        set_col(renderer, nd->mode == 0 ? COL_GREEN :
                           nd->mode == 2 ? COL_AMBER : COL_CYAN);
        font_draw_scaled_aligned(renderer, rect->x + rect->w - 30,
                                 rect->y + rect->h - 10, mode_str,
                                 0.45f, FONT_BOLD, FONT_ALIGN_RIGHT);
    }

    draw_nd_data_panel(renderer, rect, &f, nd);

    /* Border */
    set_col(renderer, COL_GRAY);
    SDL_RenderDrawRect(renderer, rect);
}

static int nd_on_event(Instrument* self, const SDL_Event* ev)
{
    NDData* nd = (NDData*)self->private_data;

    if (ev->type == SDL_KEYDOWN) {
        switch (ev->key.keysym.sym) {
            case SDLK_PLUS:  case SDLK_EQUALS:
                if (nd->range_nm < 320) nd->range_nm *= 2;
                return 1;
            case SDLK_MINUS:
                if (nd->range_nm > 10) nd->range_nm /= 2;
                return 1;
            case SDLK_m:
                nd->mode = (nd->mode + 1) % 4;  /* VOR→MAP→APP→PLN */
                return 1;
            case SDLK_w:
                nd->show_waypoints = !nd->show_waypoints;
                return 1;
            case SDLK_a:
                nd->show_airports = !nd->show_airports;
                return 1;
            case SDLK_r:
                nd->show_route = !nd->show_route;
                return 1;
            case SDLK_v:
                nd->show_navaids = !nd->show_navaids;
                return 1;
            case SDLK_c:
                nd->show_course = !nd->show_course;
                return 1;
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

    data->mode            = 1;  /* Default: MAP */
    data->range_nm        = 80;
    data->smooth_hdg      = 0.0f;
    data->fmc             = NULL;
    data->spatial_hash    = NULL;
    data->show_waypoints  = 1;
    data->show_airports   = 1;
    data->show_route      = 1;
    data->show_navaids    = 1;
    data->show_course     = 1;
    data->nearby_count    = 0;
    data->crs_selector_deg = 0.0f;
    data->crs_deviation   = 0.0f;

    inst->name         = "ND";
    inst->on_init      = nd_on_init;
    inst->on_update    = nd_on_update;
    inst->on_render    = nd_on_render;
    inst->on_event     = nd_on_event;
    inst->on_destroy   = nd_on_destroy;
    inst->private_data = data;

    return inst;
}
