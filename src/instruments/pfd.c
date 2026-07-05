/**
 * @file    pfd.c
 * @brief   Primary Flight Display implementation.
 *
 * Layout (proportional):
 *   ┌──────────────────────────────────────────────┐
 *   │              FMA (top 5%)                     │
 *   │           Thrust Ref (4%)                      │
 *   ├──────┬───────┬────────────┬───────┬───────────┤
 *   │ gap  │  SPD  │  Attitude  │  ALT  │    VSI    │
 *   │ 4%   │  10%  │    58%     │  10%  │   7%      │
 *   ├──────┴───────┴────────────┴───────┴───────────┤
 *   │          Heading tape (bottom 8%)               │
 *   └──────────────────────────────────────────────┘
 *
 * All tapes use float-based continuous scrolling — the tick marks
 * and labels slide pixel-by-pixel for smooth animation, matching
 * the behaviour of real glass-cockpit PFDs.
 */

#include "pfd.h"
#include "app.h"
#include "data/flight_data.h"
#include "utils/math_util.h"
#include "utils/font_manager.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 *  Colors (RGBA)
 * ========================================================================= */

#define COL_SKY_BLUE       0x00, 0x66, 0xCC, 255
#define COL_GROUND_BROWN   0x8B, 0x69, 0x14, 255
#define COL_HORIZON_WHITE  0xFF, 0xFF, 0xFF, 255
#define COL_BACKGROUND     0x10, 0x10, 0x18, 255
#define COL_TAPE_BG        0x20, 0x20, 0x28, 255
#define COL_TAPE_BG_ALT    0x18, 0x18, 0x22, 255
#define COL_TEXT_WHITE     0xFF, 0xFF, 0xFF, 255
#define COL_TEXT_GREEN     0x00, 0xFF, 0x40, 255
#define COL_TEXT_AMBER     0xFF, 0xC0, 0x00, 255
#define COL_TEXT_RED       0xFF, 0x30, 0x30, 255
#define COL_MAGENTA        0xFF, 0x00, 0xFF, 255
#define COL_CYAN           0x00, 0xFF, 0xFF, 255
#define COL_LINE_GRAY      0x60, 0x60, 0x68, 255
#define COL_BOX_BORDER     0x80, 0x80, 0x88, 255
#define COL_N1_GREEN       0x00, 0xD8, 0x50, 255
#define COL_N1_BG          0x10, 0x10, 0x18, 255
#define COL_FMA_ARMED      0xFF, 0xFF, 0xFF, 255   /* White = armed */
#define COL_FMA_ACTIVE     0x00, 0xFF, 0x40, 255   /* Green = active */
#define COL_FMA_MANAGED    0xFF, 0x00, 0xFF, 255   /* Magenta = managed */
#define COL_FMA_DIM        0x40, 0x40, 0x48, 255   /* Inactive/dim */
#define COL_FMA_BOX_BG     0x10, 0x18, 0x1C, 255   /* FMA box background */

/* =========================================================================
 *  Layout constants (percentages)
 * ========================================================================= */

#define FMA_H_PCT      5
#define THRUST_H_PCT   4
#define HEADING_H_PCT  8
#define GAP_W_PCT      4
#define SPD_W_PCT      10
#define ALT_W_PCT      10
#define VSI_W_PCT      7

/* =========================================================================
 *  Layout struct — recomputed each frame from instrument rect
 * ========================================================================= */

typedef struct {
    /* Vertical zones */
    int fma_y,    fma_h;
    int thrust_y, thrust_h;
    int main_y,   main_h;
    int hdg_y,    hdg_h;

    /* Horizontal columns (main area) */
    int spd_l, spd_r;       /* Speed tape left/right edge */
    int alt_l, alt_r;       /* Altitude tape left/right edge */
    int vsi_l, vsi_r;       /* VSI left/right edge */
    int att_l, att_r;       /* Attitude indicator area */
    int att_cx, att_cy;     /* Attitude indicator centre */
    int att_radius;         /* Attitude indicator radius (px) */

    /* Heading */
    int hdg_mid_y;
} PFDLayout;

static void compute_layout(const SDL_Rect* rect, PFDLayout* lay)
{
    memset(lay, 0, sizeof(*lay));

    /* ---- Vertical zones ---- */
    lay->fma_h    = rect->h * FMA_H_PCT / 100;
    lay->fma_y    = rect->y;

    lay->thrust_h = rect->h * THRUST_H_PCT / 100;
    lay->thrust_y = rect->y + lay->fma_h;

    lay->hdg_h    = rect->h * HEADING_H_PCT / 100;
    lay->hdg_y    = rect->y + rect->h - lay->hdg_h;

    lay->main_y   = rect->y + lay->fma_h + lay->thrust_h;
    lay->main_h   = lay->hdg_y - lay->main_y;
    if (lay->main_h < 100) lay->main_h = 100;

    /* ---- Horizontal columns (main area) ---- */
    int gap_w = rect->w * GAP_W_PCT / 100;
    int spd_w = rect->w * SPD_W_PCT / 100;
    int alt_w = rect->w * ALT_W_PCT / 100;
    int vsi_w = rect->w * VSI_W_PCT / 100;

    lay->spd_l = rect->x + gap_w;
    lay->spd_r = lay->spd_l + spd_w;

    lay->vsi_r = rect->x + rect->w - gap_w;
    lay->vsi_l = lay->vsi_r - vsi_w;

    lay->alt_r = lay->vsi_l;
    lay->alt_l = lay->alt_r - alt_w;

    lay->att_l = lay->spd_r;
    lay->att_r = lay->alt_l;

    /* ---- Attitude indicator centre & radius ---- */
    lay->att_cx = (lay->att_l + lay->att_r) / 2;
    lay->att_cy = lay->main_y + lay->main_h * 41 / 100;

    int max_r_h = lay->main_h * 37 / 100;
    int max_r_w = (lay->att_r - lay->att_l) * 44 / 100;
    lay->att_radius = (max_r_h < max_r_w) ? max_r_h : max_r_w;
    if (lay->att_radius < 40) lay->att_radius = 40;

    /* ---- Heading ---- */
    lay->hdg_mid_y = lay->hdg_y + lay->hdg_h / 2;
}

/* =========================================================================
 *  PFD private data
 * ========================================================================= */

typedef struct {
    /* Layout (set each frame) */
    int cx, cy;
    int att_radius;
    float scale;

    /* Smoothing state */
    float smooth_roll;
    float smooth_pitch;
    float smooth_ias;
    float smooth_alt;
    float smooth_hdg;
    float smooth_vs;
    float smooth_n1[2];

    /* Latest flight data snapshot */
    FlightDataValues fd;
} PFDData;

/* =========================================================================
 *  Drawing helper
 * ========================================================================= */

static void set_color(SDL_Renderer* r, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    SDL_SetRenderDrawColor(r, R, G, B, A);
}

/* =========================================================================
 *  Attitude indicator (artificial horizon)
 * ========================================================================= */

static void draw_attitude_indicator(SDL_Renderer* r, const PFDLayout* lay,
                                     const FlightDataValues* f)
{
    int cx = lay->att_cx;
    int cy = lay->att_cy;
    int R  = lay->att_radius;
    float roll  = f->roll_deg;
    float pitch = f->pitch_deg;
    float scale = (float)R / 35.0f;   /* ~35° visible pitch range */

    float horizon_offset = pitch * scale;
    horizon_offset = clamp_f(horizon_offset, -(float)R * 0.92f, (float)R * 0.92f);

    /* ---- Sky/ground background (matches horizon line exactly) ----
     *
     * Uses the same rotated horizon endpoints as the horizon line.
     * Cross product with the line direction determines which side a pixel is on.
     *   cross = ldx·(py - lr.y) - ldy·(px - lr.x)
     *   cross < 0 → sky,  cross > 0 → ground                     */
    {
        Vec2f hl = vec2f((float)(-R), horizon_offset);
        Vec2f hr = vec2f((float)(R),   horizon_offset);
        Vec2f lr = vec2f_rotate(hl, -roll);
        Vec2f rr = vec2f_rotate(hr, -roll);

        float ldx = rr.x - lr.x;
        float ldy = rr.y - lr.y;

        for (int dy = -R; dy <= R; dy++) {
            int y = cy + dy;
            int w = (int)sqrtf((float)(R * R - dy * dy));
            if (w <= 0) continue;

            int x0 = cx - w;
            int x1 = cx + w;

            float dist_left  = ldx * ((float)dy - lr.y) - ldy * ((float)(x0 - cx) - lr.x);
            float dist_right = ldx * ((float)dy - lr.y) - ldy * ((float)(x1 - cx) - lr.x);

            int left_sky  = (dist_left  < 0.0f);
            int right_sky = (dist_right < 0.0f);

            if (left_sky == right_sky) {
                /* Entire row is one colour */
                if (left_sky) {
                    set_color(r, 0x00, 0x66, 0xCC, 255);
                } else {
                    set_color(r, 0x8B, 0x69, 0x14, 255);
                }
                SDL_RenderDrawLine(r, x0, y, x1, y);
            } else {
                /* Row is split — find crossing x where dist == 0.
                 *   ldx·(dy - lr.y) - ldy·(x - cx - lr.x) = 0
                 * → x = cx + lr.x + ldx·(dy - lr.y) / ldy              */
                int cross_x;
                if (fabsf(ldy) > 0.0001f) {
                    cross_x = cx + (int)(lr.x + ldx * ((float)dy - lr.y) / ldy);
                } else {
                    cross_x = ((float)dy < lr.y) ? x1 + 1 : x0 - 1;
                }
                if (cross_x < x0) cross_x = x0;
                if (cross_x > x1) cross_x = x1;

                /* Left side of split */
                if (left_sky) {
                    set_color(r, 0x00, 0x66, 0xCC, 255);
                } else {
                    set_color(r, 0x8B, 0x69, 0x14, 255);
                }
                SDL_RenderDrawLine(r, x0, y, cross_x, y);
                /* Right side of split (opposite colour) */
                if (left_sky) {
                    set_color(r, 0x8B, 0x69, 0x14, 255);
                } else {
                    set_color(r, 0x00, 0x66, 0xCC, 255);
                }
                SDL_RenderDrawLine(r, cross_x, y, x1, y);
            }
        }
    }

    /* ---- Horizon line (white, 3 px thick) ---- */
    {
        Vec2f left  = vec2f((float)(-R), horizon_offset);
        Vec2f right = vec2f((float)(R),  horizon_offset);
        Vec2f lr = vec2f_rotate(left,  -roll);
        Vec2f rr = vec2f_rotate(right, -roll);

        set_color(r, COL_HORIZON_WHITE);
        draw_thick_line(r, cx + (int)lr.x, cy + (int)lr.y,
                        cx + (int)rr.x, cy + (int)rr.y, 3);
    }

    /* ---- Pitch ladder (every 5°, major at 10°) ---- */
    for (int p_deg = -30; p_deg <= 30; p_deg += 5) {
        if (p_deg == 0) continue;

        float y_offset = (float)(p_deg) * scale - horizon_offset;
        if (fabsf(y_offset) > (float)R * 0.92f) continue;

        int line_w = (p_deg % 10 == 0) ? R / 3 : R / 6;

        Vec2f left  = vec2f((float)(-line_w), -y_offset);
        Vec2f right = vec2f((float)(line_w),  -y_offset);
        Vec2f lr = vec2f_rotate(left,  -roll);
        Vec2f rr = vec2f_rotate(right, -roll);

        set_color(r, COL_HORIZON_WHITE);
        SDL_RenderDrawLine(r, cx + (int)lr.x, cy + (int)lr.y,
                           cx + (int)rr.x, cy + (int)rr.y);

        /* Numeric label for 10° bars */
        if (p_deg % 10 == 0) {
            char p_lbl[4];
            snprintf(p_lbl, sizeof(p_lbl), "%d", p_deg);
            Vec2f lbl_pos = vec2f((float)(-line_w - 18), -y_offset);
            Vec2f lr2 = vec2f_rotate(lbl_pos, -roll);
            set_color(r, COL_HORIZON_WHITE);
            font_draw_scaled_aligned(r, cx + (int)lr2.x, cy + (int)lr2.y,
                                     p_lbl, 0.40f, FONT_REGULAR, FONT_ALIGN_CENTER);
        }
    }

    /* ---- Roll scale arc (top of indicator) ---- */
    {
        set_color(r, COL_HORIZON_WHITE);
        for (int r_deg = -60; r_deg <= 60; r_deg += 10) {
            float angle = (float)(r_deg - 90);  /* 0 at top */
            float rad = (float)DEG2RAD(angle);
            float outer_r = (float)R - 8.0f;
            float inner_r = (r_deg % 30 == 0) ? (float)R - 28.0f : (float)R - 22.0f;
            int x1 = cx + (int)(outer_r * cosf(rad));
            int y1 = cy + (int)(outer_r * sinf(rad));
            int x2 = cx + (int)(inner_r * cosf(rad));
            int y2 = cy + (int)(inner_r * sinf(rad));
            SDL_RenderDrawLine(r, x1, y1, x2, y2);
        }
    }

    /* ---- Roll pointer (fixed triangle at top) ---- */
    {
        set_color(r, COL_HORIZON_WHITE);
        int tri_x = cx;
        int tri_y = cy - R + 4;
        SDL_RenderDrawLine(r, tri_x, tri_y, tri_x - 8, tri_y - 12);
        SDL_RenderDrawLine(r, tri_x, tri_y, tri_x + 8, tri_y - 12);
        SDL_RenderDrawLine(r, tri_x - 8, tri_y - 12, tri_x + 8, tri_y - 12);
    }

    /* ---- Aircraft reference symbol (centre, fixed) ---- */
    {
        /* Wings */
        set_color(r, COL_TEXT_AMBER);
        SDL_RenderDrawLine(r, cx - 55, cy, cx - 22, cy);
        SDL_RenderDrawLine(r, cx + 22, cy, cx + 55, cy);
        /* Fuselage dot */
        draw_filled_circle(r, cx, cy, 4);
        /* Nose */
        set_color(r, COL_HORIZON_WHITE);
        SDL_RenderDrawLine(r, cx, cy, cx, cy - 16);
    }

    /* ---- Circle border ---- */
    set_color(r, COL_LINE_GRAY);
    for (int i = 0; i < 72; i++) {
        float a1 = (float)i * 2.0f * (float)M_PI / 72.0f;
        float a2 = (float)(i + 1) * 2.0f * (float)M_PI / 72.0f;
        SDL_RenderDrawLine(r,
                           cx + (int)((float)R * cosf(a1)),
                           cy + (int)((float)R * sinf(a1)),
                           cx + (int)((float)R * cosf(a2)),
                           cy + (int)((float)R * sinf(a2)));
    }
}

/* =========================================================================
 *  Airspeed tape — float-based continuous scrolling
 * ========================================================================= */

static void draw_airspeed_tape(SDL_Renderer* r, const PFDLayout* lay,
                                const FlightDataValues* f)
{
    int tape_l   = lay->spd_l;
    int tape_r   = lay->spd_r;
    int tape_w   = tape_r - tape_l;
    int tape_mid = lay->main_y + lay->main_h / 2;
    int tape_h   = lay->main_h * 74 / 100;
    int tape_y   = lay->main_y + (lay->main_h - tape_h) / 2;

    float ias    = f->ias_kts;          /* CONTINUOUS float — key fix */
    float px_kt  = 5.0f;               /* pixels per knot */

    /* ---- Background ---- */
    set_color(r, COL_TAPE_BG);
    SDL_Rect bg = { tape_l, tape_y, tape_w, tape_h };
    SDL_RenderFillRect(r, &bg);
    set_color(r, COL_BOX_BORDER);
    SDL_RenderDrawRect(r, &bg);

    /* ---- Tick marks & labels (continuous scrolling) ---- */
    float ias_floor = floorf(ias / 10.0f) * 10.0f;
    for (float spd = ias_floor - 80.0f; spd <= ias + 80.0f; spd += 10.0f) {
        float y_pos = (float)tape_mid - (spd - ias) * px_kt;
        if (y_pos < (float)tape_y + 3.0f || y_pos > (float)(tape_y + tape_h) - 3.0f)
            continue;

        int tick_w = ((int)spd % 20 == 0) ? tape_w * 2 / 3 : tape_w / 3;
        set_color(r, COL_HORIZON_WHITE);
        SDL_RenderDrawLine(r, tape_r - tick_w, (int)y_pos,
                           tape_r, (int)y_pos);

        if ((int)spd % 20 == 0) {
            char lbl[5];
            snprintf(lbl, sizeof(lbl), "%d", (int)spd);
            set_color(r, COL_HORIZON_WHITE);
            font_draw_scaled_aligned(r, tape_l + 2, (int)y_pos,
                                     lbl, 0.48f, FONT_REGULAR, FONT_ALIGN_LEFT);
        }
    }

    /* ---- Current IAS window (fixed centre box) ---- */
    int box_h = 32;
    set_color(r, COL_TAPE_BG);
    SDL_Rect ias_box = { tape_l + 1, tape_mid - box_h / 2,
                          tape_w - 2, box_h };
    SDL_RenderFillRect(r, &ias_box);
    set_color(r, COL_HORIZON_WHITE);
    SDL_RenderDrawRect(r, &ias_box);

    {
        char ias_str[8];
        snprintf(ias_str, sizeof(ias_str), "%d", (int)ias);
        set_color(r, COL_HORIZON_WHITE);
        draw_text_simple(r, tape_l + tape_w / 2, tape_mid, ias_str, 0.85f);
    }

    /* ---- "SPD" label below box ---- */
    set_color(r, COL_HORIZON_WHITE);
    draw_text_simple(r, tape_l + tape_w / 2,
                     tape_mid + box_h / 2 + 7, "SPD", 0.55f);

    /* ---- Mach number ---- */
    if (f->mach > 0.40f) {
        char mach_str[10];
        snprintf(mach_str, sizeof(mach_str), "M%.3f", (double)f->mach);
        set_color(r, COL_HORIZON_WHITE);
        draw_text_simple(r, tape_l + tape_w / 2,
                         tape_mid + box_h / 2 + 22, mach_str, 0.48f);
    }

    /* ---- AP speed bug (magenta) ---- */
    if (f->ap_spd > 0.0f) {
        float bug_y = (float)tape_mid - (f->ap_spd - ias) * px_kt;
        if (bug_y > (float)tape_y && bug_y < (float)(tape_y + tape_h)) {
            set_color(r, COL_MAGENTA);
            /* Triangle on right edge */
            SDL_RenderDrawLine(r, tape_r + 1, (int)bug_y - 4,
                               tape_r + 1, (int)bug_y + 4);
            SDL_RenderDrawLine(r, tape_r + 1, (int)bug_y,
                               tape_r + 7, (int)bug_y);
        }
    }
}

/* =========================================================================
 *  Altitude tape — float-based continuous scrolling
 * ========================================================================= */

static void draw_altitude_tape(SDL_Renderer* r, const PFDLayout* lay,
                                const FlightDataValues* f)
{
    int tape_l   = lay->alt_l;
    int tape_r   = lay->alt_r;
    int tape_w   = tape_r - tape_l;
    int tape_mid = lay->main_y + lay->main_h / 2;
    int tape_h   = lay->main_h * 74 / 100;
    int tape_y   = lay->main_y + (lay->main_h - tape_h) / 2;

    float alt    = f->altitude_ft;       /* CONTINUOUS float — key fix */
    float px_ft  = 0.6f;                /* pixels per foot */

    /* ---- Background ---- */
    set_color(r, COL_TAPE_BG);
    SDL_Rect bg = { tape_l, tape_y, tape_w, tape_h };
    SDL_RenderFillRect(r, &bg);
    set_color(r, COL_BOX_BORDER);
    SDL_RenderDrawRect(r, &bg);

    /* ---- Alternating bands + tick marks (continuous scrolling) ---- */
    float alt_floor = floorf(alt / 100.0f) * 100.0f;
    for (float a = alt_floor - 700.0f; a <= alt + 700.0f; a += 100.0f) {
        float y_pos = (float)tape_mid - (a - alt) * px_ft;
        if (y_pos < (float)tape_y - 3.0f || y_pos > (float)(tape_y + tape_h) + 3.0f)
            continue;

        /* Alternating background bands */
        int band_h = (int)(100.0f * px_ft) + 1;
        if (((int)a / 100) % 2 == 0)
            set_color(r, COL_TAPE_BG);
        else
            set_color(r, COL_TAPE_BG_ALT);
        SDL_Rect band = { tape_l, (int)y_pos - band_h / 2, tape_w, band_h };
        SDL_RenderFillRect(r, &band);

        /* Tick line */
        set_color(r, COL_HORIZON_WHITE);
        int tick_w = ((int)a % 500 == 0) ? tape_w * 2 / 3 : tape_w / 3;
        SDL_RenderDrawLine(r, tape_l, (int)y_pos, tape_l + tick_w, (int)y_pos);

        /* Label (last 3 digits) */
        if ((int)a % 200 == 0) {
            int a_mod = (int)a % 1000;
            if (a_mod < 0) a_mod += 1000;
            char lbl[6];
            snprintf(lbl, sizeof(lbl), "%03d", a_mod);
            set_color(r, COL_HORIZON_WHITE);
            font_draw_scaled_aligned(r, tape_r - 2, (int)y_pos,
                                     lbl, 0.45f, FONT_REGULAR, FONT_ALIGN_RIGHT);
        }
    }

    /* ---- Current ALT window (fixed centre box) ---- */
    int box_h = 32;
    set_color(r, COL_TAPE_BG);
    SDL_Rect alt_box = { tape_l + 1, tape_mid - box_h / 2,
                          tape_w - 2, box_h };
    SDL_RenderFillRect(r, &alt_box);
    set_color(r, COL_HORIZON_WHITE);
    SDL_RenderDrawRect(r, &alt_box);

    {
        char alt_str[8];
        snprintf(alt_str, sizeof(alt_str), "%d", (int)alt);
        set_color(r, COL_HORIZON_WHITE);
        draw_text_simple(r, tape_l + tape_w / 2, tape_mid, alt_str, 0.85f);
    }

    /* ---- "ALT" label below box ---- */
    set_color(r, COL_HORIZON_WHITE);
    draw_text_simple(r, tape_l + tape_w / 2,
                     tape_mid + box_h / 2 + 7, "ALT", 0.55f);

    /* ---- Barometric setting ---- */
    {
        char baro_str[32];
        snprintf(baro_str, sizeof(baro_str), "%.2f inHg", (double)f->baro_setting_inhg);
        set_color(r, COL_TEXT_GREEN);
        draw_text_simple(r, tape_l + tape_w / 2,
                         tape_mid + box_h / 2 + 22, baro_str, 0.48f);
    }

    /* ---- Radio altitude (when < 2500 ft AGL) ---- */
    if (f->altitude_agl_ft < 2500.0f && f->altitude_agl_ft > -50.0f) {
        char ra_str[16];
        snprintf(ra_str, sizeof(ra_str), "RADIO %.0f", (double)f->altitude_agl_ft);
        set_color(r, COL_TEXT_GREEN);
        draw_text_simple(r, tape_l + tape_w / 2,
                         tape_mid + box_h / 2 + 36, ra_str, 0.48f);
    }

    /* ---- AP altitude bug (magenta) ---- */
    if (f->ap_alt > 0.0f) {
        float bug_y = (float)tape_mid - (f->ap_alt - alt) * px_ft;
        if (bug_y > (float)tape_y && bug_y < (float)(tape_y + tape_h)) {
            set_color(r, COL_MAGENTA);
            SDL_RenderDrawLine(r, tape_l - 6, (int)bug_y, tape_l - 1, (int)bug_y);
            SDL_RenderDrawLine(r, tape_l - 1, (int)bug_y - 4,
                               tape_l - 1, (int)bug_y + 4);
        }
    }
}

/* =========================================================================
 *  VSI — Vertical Speed Indicator (right of altitude tape)
 * ========================================================================= */

static void draw_vsi(SDL_Renderer* r, const PFDLayout* lay,
                      const FlightDataValues* f)
{
    int vsi_l   = lay->vsi_l + 1;
    int vsi_r   = lay->vsi_r - 1;
    int vsi_w   = vsi_r - vsi_l;
    int vsi_mid = lay->main_y + lay->main_h / 2;
    int vsi_h   = lay->main_h * 70 / 100;
    int vsi_y   = lay->main_y + (lay->main_h - vsi_h) / 2;

    float vs_fpm    = f->vs_fpm;
    float px_kfpm   = (float)vsi_h / 12.0f;   /* 12 kfpm total range (±6) */

    /* ---- Background ---- */
    set_color(r, COL_TAPE_BG);
    SDL_Rect bg = { vsi_l, vsi_y, vsi_w, vsi_h };
    SDL_RenderFillRect(r, &bg);
    set_color(r, COL_BOX_BORDER);
    SDL_RenderDrawRect(r, &bg);

    /* ---- Scale tick marks & labels ---- */
    static const int vs_ticks[] = { -6, -4, -2, -1, 1, 2, 4, 6 };
    for (int i = 0; i < 8; i++) {
        int v = vs_ticks[i];
        float y_pos = (float)vsi_mid - (float)v * px_kfpm;
        if (y_pos < (float)vsi_y + 2.0f || y_pos > (float)(vsi_y + vsi_h) - 2.0f)
            continue;

        int tick_w = (abs(v) % 2 == 0) ? vsi_w * 2 / 3 : vsi_w / 3;
        set_color(r, COL_HORIZON_WHITE);
        SDL_RenderDrawLine(r, vsi_l + 2, (int)y_pos,
                           vsi_l + 2 + tick_w, (int)y_pos);

        /* Label at even thousands */
        if (abs(v) % 2 == 0) {
            char lbl[4];
            snprintf(lbl, sizeof(lbl), "%d", abs(v));
            set_color(r, COL_HORIZON_WHITE);
            font_draw_scaled_aligned(r, vsi_r - 1, (int)y_pos,
                                     lbl, 0.42f, FONT_REGULAR, FONT_ALIGN_RIGHT);
        }
    }

    /* ---- Zero line ---- */
    set_color(r, COL_HORIZON_WHITE);
    SDL_RenderDrawLine(r, vsi_l + 2, vsi_mid, vsi_r - 2, vsi_mid);

    /* ---- Pointer (moving triangle on left) ---- */
    float vs_kfpm = clamp_f(vs_fpm / 1000.0f, -5.8f, 5.8f);
    int ptr_y = (int)((float)vsi_mid - vs_kfpm * px_kfpm);
    ptr_y = clamp_i(ptr_y, vsi_y + 1, vsi_y + vsi_h - 1);

    set_color(r, COL_CYAN);
    SDL_RenderDrawLine(r, vsi_l + 2, ptr_y - 4, vsi_l + 8, ptr_y);
    SDL_RenderDrawLine(r, vsi_l + 2, ptr_y + 4, vsi_l + 8, ptr_y);
    SDL_RenderDrawLine(r, vsi_l + 8, ptr_y, vsi_l + 2, ptr_y - 4);

    /* ---- Digital readout ---- */
    char vs_str[16];
    int vs_abs = (int)fabsf(vs_fpm);
    if (vs_fpm >= 50.0f)
        snprintf(vs_str, sizeof(vs_str), "+%d", vs_abs);
    else if (vs_fpm <= -50.0f)
        snprintf(vs_str, sizeof(vs_str), "-%d", vs_abs);
    else
        snprintf(vs_str, sizeof(vs_str), "0");

    int readout_y = ptr_y;
    if (readout_y < vsi_y + 10) readout_y = vsi_y + 10;
    if (readout_y > vsi_y + vsi_h - 10) readout_y = vsi_y + vsi_h - 10;

    set_color(r, COL_TEXT_GREEN);
    font_draw_scaled_aligned(r, vsi_l + vsi_w / 2, readout_y - 10,
                             vs_str, 0.52f, FONT_BOLD, FONT_ALIGN_CENTER);

    /* ---- "V/S" label at bottom ---- */
    set_color(r, COL_HORIZON_WHITE);
    draw_text_simple(r, vsi_l + vsi_w / 2,
                     vsi_y + vsi_h + 8, "V/S", 0.42f);
}

/* =========================================================================
 *  Heading tape (bottom) — improved compass-rose style
 * ========================================================================= */

static void draw_heading_tape(SDL_Renderer* r, const SDL_Rect* rect,
                               const PFDLayout* lay, const FlightDataValues* f)
{
    int tape_y     = lay->hdg_y;
    int tape_h     = lay->hdg_h;
    int tape_mid_x = rect->x + rect->w / 2;
    int tape_mid_y = lay->hdg_mid_y;
    float hdg      = f->heading_true_deg;
    float px_deg   = 6.0f;

    /* ---- Background ---- */
    set_color(r, COL_TAPE_BG);
    SDL_Rect bg = { rect->x, tape_y, rect->w, tape_h };
    SDL_RenderFillRect(r, &bg);
    set_color(r, COL_BOX_BORDER);
    SDL_RenderDrawRect(r, &bg);

    /* ---- Ticks every 5°, major every 10°, labelled at 30° steps ---- */
    /*     Continuous scrolling: tick x = center + (heading_value - hdg) * px_deg */
    float hdg_floor = floorf(hdg / 5.0f) * 5.0f;
    for (float h = hdg_floor - 100.0f; h <= hdg + 100.0f; h += 5.0f) {
        float h_val = (float)norm_angle_360((double)h);
        float delta = h - hdg;     /* degrees from current heading */
        int x = tape_mid_x + (int)(delta * px_deg);
        if (x < rect->x + 3 || x > rect->x + rect->w - 3) continue;

        int abs_h = (int)h;
        int is_major = (abs_h % 10 == 0);
        int tick_h   = is_major ? tape_h * 2 / 3 : tape_h / 3;

        set_color(r, COL_HORIZON_WHITE);
        SDL_RenderDrawLine(r, x, tape_y, x, tape_y + tick_h);

        /* Label at every 30° */
        if (abs_h % 30 == 0) {
            int hdg_int = (int)h_val;
            if (hdg_int == 360) hdg_int = 0;

            char lbl[4];
            if (hdg_int == 0)       snprintf(lbl, sizeof(lbl), "N");
            else if (hdg_int == 90)  snprintf(lbl, sizeof(lbl), "E");
            else if (hdg_int == 180) snprintf(lbl, sizeof(lbl), "S");
            else if (hdg_int == 270) snprintf(lbl, sizeof(lbl), "W");
            else                     snprintf(lbl, sizeof(lbl), "%02d", hdg_int / 10);

            int is_cardinal = (hdg_int == 0 || hdg_int == 90 ||
                               hdg_int == 180 || hdg_int == 270);
            set_color(r, COL_HORIZON_WHITE);
            font_draw_scaled_aligned(r, x, tape_y + tape_h - 2,
                                     lbl, is_cardinal ? 0.55f : 0.48f,
                                     FONT_REGULAR, FONT_ALIGN_CENTER);
        }
    }

    /* ---- Current heading box (centre, on top of tape) ---- */
    {
        int box_w = 56;
        int box_h = tape_h - 1;
        set_color(r, COL_BACKGROUND);
        SDL_Rect hdg_box = { tape_mid_x - box_w / 2, tape_y + 1,
                              box_w, box_h - 1 };
        SDL_RenderFillRect(r, &hdg_box);
        set_color(r, COL_HORIZON_WHITE);
        SDL_RenderDrawRect(r, &hdg_box);

        /* Triangle marker at top */
        SDL_RenderDrawLine(r, tape_mid_x, tape_y + 1,
                           tape_mid_x - 5, tape_y - 6);
        SDL_RenderDrawLine(r, tape_mid_x, tape_y + 1,
                           tape_mid_x + 5, tape_y - 6);
        SDL_RenderDrawLine(r, tape_mid_x - 5, tape_y - 6,
                           tape_mid_x + 5, tape_y - 6);
    }

    /* Heading value in box */
    {
        char hdg_str[8];
        snprintf(hdg_str, sizeof(hdg_str), "%03d", (int)hdg);
        set_color(r, COL_HORIZON_WHITE);
        draw_text_simple(r, tape_mid_x, tape_mid_y, hdg_str, 0.8f);
    }

    /* ---- "HDG" label ---- */
    set_color(r, COL_LINE_GRAY);
    draw_text_simple(r, tape_mid_x, tape_mid_y + 14, "HDG", 0.40f);

    /* ---- AP heading bug (magenta diamond) ---- */
    if (f->ap_hdg >= 0.0f) {
        float delta = (float)norm_angle_360((double)(f->ap_hdg - hdg));
        if (delta > 180.0f) delta -= 360.0f;
        int bug_x = tape_mid_x + (int)(delta * px_deg);
        if (bug_x > rect->x + 6 && bug_x < rect->x + rect->w - 6) {
            set_color(r, COL_MAGENTA);
            /* Diamond on top of tape border */
            SDL_RenderDrawLine(r, bug_x, tape_y + 1, bug_x + 4, tape_y - 6);
            SDL_RenderDrawLine(r, bug_x + 4, tape_y - 6, bug_x, tape_y - 12);
            SDL_RenderDrawLine(r, bug_x, tape_y - 12, bug_x - 4, tape_y - 6);
            SDL_RenderDrawLine(r, bug_x - 4, tape_y - 6, bug_x, tape_y + 1);
        }
    }
}

/* =========================================================================
 *  Thrust reference (N1 gauges) — above attitude indicator
 * ========================================================================= */

static void draw_thrust_ref(SDL_Renderer* r, const SDL_Rect* rect,
                             const PFDLayout* lay, const FlightDataValues* f)
{
    int ty    = lay->thrust_y;
    int th    = lay->thrust_h;
    int mid_x = rect->x + rect->w / 2;

    /* Two engine gauges side by side */
    int gauge_w = rect->w * 30 / 100;   /* Each gauge 30% of width */
    int gap     = rect->w * 8 / 100;    /* Gap between gauges */
    int bar_h   = th * 55 / 100;        /* Bar height */
    int bar_y   = ty + (th - bar_h) / 2;

    /* ---- Section background ---- */
    set_color(r, COL_BACKGROUND);
    SDL_Rect sec = { rect->x, ty, rect->w, th };
    SDL_RenderFillRect(r, &sec);
    set_color(r, COL_LINE_GRAY);
    SDL_RenderDrawLine(r, rect->x, ty + th - 1, rect->x + rect->w, ty + th - 1);

    for (int eng = 0; eng < 2; eng++) {
        int gx = (eng == 0) ? mid_x - gap / 2 - gauge_w
                            : mid_x + gap / 2;

        float n1_pct = clamp_f(f->n1_pct[eng], 0.0f, 110.0f);

        /* Engine label */
        char eng_lbl[8];
        snprintf(eng_lbl, sizeof(eng_lbl), "N1 %d", eng + 1);
        set_color(r, COL_TEXT_WHITE);
        font_draw_scaled_aligned(r, gx + gauge_w / 2, ty + 2,
                                 eng_lbl, 0.42f, FONT_REGULAR, FONT_ALIGN_CENTER);

        /* Bar background */
        int bar_w = gauge_w - 10;
        set_color(r, COL_N1_BG);
        SDL_Rect bar_bg = { gx + 5, bar_y, bar_w, bar_h };
        SDL_RenderFillRect(r, &bar_bg);
        set_color(r, COL_LINE_GRAY);
        SDL_RenderDrawRect(r, &bar_bg);

        /* Filled portion */
        int fill_w = (int)((float)bar_w * n1_pct / 110.0f);
        if (fill_w > 0) {
            if (n1_pct > 100.0f)
                set_color(r, COL_TEXT_AMBER);
            else
                set_color(r, COL_N1_GREEN);
            SDL_Rect fill = { gx + 5, bar_y + 1, fill_w, bar_h - 2 };
            SDL_RenderFillRect(r, &fill);
        }

        /* Reference line at 100% */
        int ref_x = gx + 5 + bar_w * 100 / 110;
        set_color(r, COL_TEXT_RED);
        SDL_RenderDrawLine(r, ref_x, bar_y - 1, ref_x, bar_y + bar_h + 1);

        /* Digital N1% */
        char n1_str[10];
        snprintf(n1_str, sizeof(n1_str), "%.1f%%", (double)n1_pct);
        if (n1_pct > 100.0f) {
            set_color(r, 0xFF, 0xC0, 0x00, 255);
        } else {
            set_color(r, 0x00, 0xFF, 0x40, 255);
        }
        font_draw_scaled_aligned(r, gx + gauge_w / 2, bar_y + bar_h + 2,
                                 n1_str, 0.48f, FONT_BOLD, FONT_ALIGN_CENTER);
    }
}

/* =========================================================================
 *  FMA — Flight Mode Annunciator (top strip)
 * ========================================================================= */

static void draw_fma(SDL_Renderer* r, const SDL_Rect* rect,
                      const PFDLayout* lay, const FlightDataValues* f)
{
    int fy = lay->fma_y;
    int fh = lay->fma_h;
    int mid_y = fy + fh / 2;

    /* ---- Background ---- */
    set_color(r, COL_TAPE_BG);
    SDL_Rect bg = { rect->x, fy, rect->w, fh };
    SDL_RenderFillRect(r, &bg);

    /* Separator line below FMA */
    set_color(r, COL_LINE_GRAY);
    SDL_RenderDrawLine(r, rect->x, fy + fh - 1, rect->x + rect->w, fy + fh - 1);

    /* Column layout — 4 equal columns */
    int col_w = rect->w / 4;

    /* === Column 1: Auto-throttle === */
    {
        int cx = rect->x + col_w / 2;
        int bw = col_w - 16;
        int bx = cx - bw / 2;
        int bh = fh - 6;
        int by = fy + 3;

        if (f->ap_athr_engaged) {
            set_color(r, COL_FMA_BOX_BG);
            SDL_Rect box = { bx, by, bw, bh };
            SDL_RenderFillRect(r, &box);
            set_color(r, COL_FMA_ACTIVE);
        } else {
            set_color(r, COL_FMA_DIM);
        }
        font_draw_scaled_aligned(r, cx, mid_y, "A/THR", 0.55f,
                                 FONT_REGULAR, FONT_ALIGN_CENTER);
    }

    /* === Column 2: Roll mode === */
    {
        int cx = rect->x + col_w + col_w / 2;
        int bw = col_w - 16;
        int bx = cx - bw / 2;
        int bh = fh - 6;
        int by = fy + 3;

        if (f->ap_engaged) {
            set_color(r, COL_FMA_BOX_BG);
            SDL_Rect box = { bx, by, bw, bh };
            SDL_RenderFillRect(r, &box);
            set_color(r, COL_MAGENTA);
            font_draw_scaled_aligned(r, cx, mid_y, "HDG", 0.55f,
                                     FONT_REGULAR, FONT_ALIGN_CENTER);
        } else {
            set_color(r, COL_FMA_DIM);
            font_draw_scaled_aligned(r, cx, mid_y, "---", 0.55f,
                                     FONT_REGULAR, FONT_ALIGN_CENTER);
        }
    }

    /* === Column 3: Pitch mode === */
    {
        int cx = rect->x + col_w * 2 + col_w / 2;
        int bw = col_w - 16;
        int bx = cx - bw / 2;
        int bh = fh - 6;
        int by = fy + 3;

        const char* pitch_mode = "---";
        if (f->ap_engaged) {
            if (f->ap_vs > 100.0f || f->ap_vs < -100.0f)
                pitch_mode = "V/S";
            else if (f->ap_alt > 0.0f)
                pitch_mode = "ALT";
            else
                pitch_mode = "ALT";
        }

        if (f->ap_engaged) {
            set_color(r, COL_FMA_BOX_BG);
            SDL_Rect box = { bx, by, bw, bh };
            SDL_RenderFillRect(r, &box);
            set_color(r, COL_MAGENTA);
        } else {
            set_color(r, COL_FMA_DIM);
        }
        font_draw_scaled_aligned(r, cx, mid_y, pitch_mode, 0.55f,
                                 FONT_REGULAR, FONT_ALIGN_CENTER);
    }

    /* === Column 4: AP status === */
    {
        int cx = rect->x + col_w * 3 + col_w / 2;
        int bw = col_w - 16;
        int bx = cx - bw / 2;
        int bh = fh - 6;
        int by = fy + 3;

        if (f->ap_engaged) {
            set_color(r, COL_FMA_BOX_BG);
            SDL_Rect box = { bx, by, bw, bh };
            SDL_RenderFillRect(r, &box);
            set_color(r, COL_FMA_ACTIVE);
            font_draw_scaled_aligned(r, cx, mid_y, "CMD", 0.55f,
                                     FONT_REGULAR, FONT_ALIGN_CENTER);
        } else {
            set_color(r, COL_FMA_DIM);
            font_draw_scaled_aligned(r, cx, mid_y, "---", 0.55f,
                                     FONT_REGULAR, FONT_ALIGN_CENTER);
        }
    }

    /* === Master Warning / Caution (rightmost overlay) === */
    {
        int warn_x = rect->x + rect->w * 92 / 100;
        if (f->master_warning) {
            int blink = ((SDL_GetTicks() / 500) % 2);
            if (blink) {
                set_color(r, COL_TEXT_RED);
                font_draw_scaled_aligned(r, warn_x, mid_y, "WARN", 0.55f,
                                         FONT_BOLD, FONT_ALIGN_RIGHT);
            }
        } else if (f->master_caution) {
            set_color(r, COL_TEXT_AMBER);
            font_draw_scaled_aligned(r, warn_x, mid_y, "CAUT", 0.55f,
                                     FONT_BOLD, FONT_ALIGN_RIGHT);
        }
    }
}

/* =========================================================================
 *  Instrument interface implementation
 * ========================================================================= */

static void pfd_on_init(Instrument* self, App* app)
{
    (void)app;
    PFDData* p = (PFDData*)self->private_data;

    PFDLayout lay;
    compute_layout(&self->rect, &lay);

    p->cx         = lay.att_cx;
    p->cy         = lay.att_cy;
    p->att_radius = lay.att_radius;
    p->scale      = (float)lay.att_radius / 35.0f;

    LOG_DEBUG("PFD initialized: centre=(%d,%d) radius=%d",
              p->cx, p->cy, p->att_radius);
}

static void pfd_on_update(Instrument* self, const FlightData* fd, float dt)
{
    (void)dt;
    PFDData* p = (PFDData*)self->private_data;
    if (!fd) return;

    flight_data_snapshot((FlightData*)fd, &p->fd);

    /* Smooth key values for fluid animation.
     * Use angle-aware smooth for heading (0–360°) to handle 0°/360° wrap. */
    float alpha = 0.18f;
    p->smooth_roll  = exp_smooth(p->smooth_roll,  p->fd.roll_deg,        alpha);
    p->smooth_pitch = exp_smooth(p->smooth_pitch, p->fd.pitch_deg,       alpha);
    p->smooth_ias   = exp_smooth(p->smooth_ias,   p->fd.ias_kts,         alpha);
    p->smooth_alt   = exp_smooth(p->smooth_alt,   p->fd.altitude_ft,     alpha);
    p->smooth_hdg   = exp_smooth_angle(p->smooth_hdg,   p->fd.heading_true_deg, alpha);
    p->smooth_vs    = exp_smooth(p->smooth_vs,    p->fd.vs_fpm,          alpha);
    p->smooth_n1[0] = exp_smooth(p->smooth_n1[0], p->fd.n1_pct[0],       alpha);
    p->smooth_n1[1] = exp_smooth(p->smooth_n1[1], p->fd.n1_pct[1],       alpha);
}

static void pfd_on_render(Instrument* self, SDL_Renderer* renderer)
{
    PFDData* p = (PFDData*)self->private_data;
    const SDL_Rect* rect = &self->rect;

    /* ---- Recompute layout (window may have been resized) ---- */
    PFDLayout lay;
    compute_layout(rect, &lay);

    /* ---- Build display data with smoothed values ---- */
    FlightDataValues f = p->fd;
    f.roll_deg         = p->smooth_roll;
    f.pitch_deg        = p->smooth_pitch;
    f.ias_kts          = p->smooth_ias;
    f.altitude_ft      = p->smooth_alt;
    f.heading_true_deg = p->smooth_hdg;
    f.vs_fpm           = p->smooth_vs;
    f.n1_pct[0]        = p->smooth_n1[0];
    f.n1_pct[1]        = p->smooth_n1[1];

    /* Sync PFDData layout fields (for attitude indicator compat) */
    p->cx         = lay.att_cx;
    p->cy         = lay.att_cy;
    p->att_radius = lay.att_radius;
    p->scale      = (float)lay.att_radius / 35.0f;

    /* ---- Background fill for entire instrument ---- */
    set_color(renderer, COL_BACKGROUND);
    SDL_RenderFillRect(renderer, rect);

    /* ---- Draw all PFD components (top → bottom, left → right) ---- */
    draw_fma(renderer, rect, &lay, &f);
    draw_thrust_ref(renderer, rect, &lay, &f);
    draw_attitude_indicator(renderer, &lay, &f);
    draw_airspeed_tape(renderer, &lay, &f);
    draw_altitude_tape(renderer, &lay, &f);
    draw_vsi(renderer, &lay, &f);
    draw_heading_tape(renderer, rect, &lay, &f);
}

static int pfd_on_event(Instrument* self, const SDL_Event* ev)
{
    (void)self;
    (void)ev;
    return 0;  /* PFD does not consume events directly */
}

static void pfd_on_destroy(Instrument* self)
{
    if (self && self->private_data) {
        free(self->private_data);
        self->private_data = NULL;
    }
}

/* =========================================================================
 *  Constructor
 * ========================================================================= */

Instrument* pfd_create(void)
{
    Instrument* inst = calloc(1, sizeof(Instrument));
    if (!inst) {
        LOG_ERROR("Out of memory creating PFD instrument");
        return NULL;
    }

    PFDData* data = calloc(1, sizeof(PFDData));
    if (!data) {
        LOG_ERROR("Out of memory creating PFD private data");
        free(inst);
        return NULL;
    }

    /* Smoothers initialised to 0.0 by calloc */

    inst->name         = "PFD";
    inst->on_init      = pfd_on_init;
    inst->on_update    = pfd_on_update;
    inst->on_render    = pfd_on_render;
    inst->on_event     = pfd_on_event;
    inst->on_destroy   = pfd_on_destroy;
    inst->private_data = data;

    return inst;
}
