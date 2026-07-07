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

#define FMA_H_PCT      6
#define THRUST_H_PCT   12
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

    lay->hdg_h    = 70; /* Fixed height for compass area as requested */
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

    /* For robust VSI calculation */
    float last_alt_for_vsi;
    int   last_alt_valid;

    /* For robust Pitch Rate calculation */
    float last_pitch_for_rate;
    int   last_pitch_valid;
    float smooth_pitch_rate;

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

    /* ---- Pitch ladder (every 2.5°, major at 10°, mid at 5°) ---- */
    for (float p_deg_f = -180.0f; p_deg_f <= 180.0f; p_deg_f += 2.5f) {
        if (fabsf(p_deg_f) < 0.1f) continue;
        
        /* Only display range current_pitch - 20 to current_pitch + 20 */
        if (p_deg_f < pitch - 20.0f || p_deg_f > pitch + 20.0f) continue;

        float y_offset = p_deg_f * scale - horizon_offset;
        
        int is_major = (fmodf(fabsf(p_deg_f), 10.0f) < 0.1f);
        int is_mid = (fmodf(fabsf(p_deg_f), 5.0f) < 0.1f);
        int line_w;
        if (is_major) line_w = R / 3;
        else if (is_mid) line_w = R / 5;
        else line_w = R / 8;

        Vec2f left  = vec2f((float)(-line_w), -y_offset);
        Vec2f right = vec2f((float)(line_w),  -y_offset);
        Vec2f lr = vec2f_rotate(left,  -roll);
        Vec2f rr = vec2f_rotate(right, -roll);

        set_color(r, COL_HORIZON_WHITE);
        SDL_RenderDrawLine(r, cx + (int)lr.x, cy + (int)lr.y,
                           cx + (int)rr.x, cy + (int)rr.y);
        /* If pitch is negative, draw downward ticks at ends of lines */
        if (p_deg_f < 0.0f && is_major) {
            Vec2f l_tick = vec2f((float)(-line_w), -y_offset + 5.0f);
            Vec2f r_tick = vec2f((float)(line_w),  -y_offset + 5.0f);
            Vec2f ltr = vec2f_rotate(l_tick, -roll);
            Vec2f rtr = vec2f_rotate(r_tick, -roll);
            SDL_RenderDrawLine(r, cx + (int)lr.x, cy + (int)lr.y, cx + (int)ltr.x, cy + (int)ltr.y);
            SDL_RenderDrawLine(r, cx + (int)rr.x, cy + (int)rr.y, cx + (int)rtr.x, cy + (int)rtr.y);
        }

        /* Numeric label for 10° bars */
        if (is_major) {
            char p_lbl[16];
            snprintf(p_lbl, sizeof(p_lbl), "%d", (int)fabsf(p_deg_f));
            Vec2f lbl_pos = vec2f((float)(-line_w - 18), -y_offset);
            Vec2f lr2 = vec2f_rotate(lbl_pos, -roll);
            set_color(r, COL_HORIZON_WHITE);
            font_draw_scaled_aligned(r, cx + (int)lr2.x, cy + (int)lr2.y,
                                     p_lbl, 0.40f, FONT_REGULAR, FONT_ALIGN_CENTER);
            
            Vec2f lbl_pos_r = vec2f((float)(line_w + 18), -y_offset);
            Vec2f rr2 = vec2f_rotate(lbl_pos_r, -roll);
            font_draw_scaled_aligned(r, cx + (int)rr2.x, cy + (int)rr2.y,
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
        /* Top solid triangle (static pointer for roll scale) */
        int tri_x = cx;
        int tri_y = cy - R + 4;
        SDL_RenderDrawLine(r, tri_x, tri_y, tri_x - 8, tri_y - 12);
        SDL_RenderDrawLine(r, tri_x, tri_y, tri_x + 8, tri_y - 12);
        SDL_RenderDrawLine(r, tri_x - 8, tri_y - 12, tri_x + 8, tri_y - 12);
    }

    /* ---- Roll pointer (dynamic hollow triangle at top, rotates with roll) ---- */
    {
        set_color(r, COL_HORIZON_WHITE);
        float rad = (float)DEG2RAD(-90.0 - roll);
        float ptr_r = (float)R - 30.0f;
        int tri_cx = cx + (int)(ptr_r * cosf(rad));
        int tri_cy = cy + (int)(ptr_r * sinf(rad));
        
        Vec2f p1 = vec2f_rotate(vec2f(0.0f, -12.0f), -roll);
        Vec2f p2 = vec2f_rotate(vec2f(-8.0f, 0.0f), -roll);
        Vec2f p3 = vec2f_rotate(vec2f(8.0f, 0.0f), -roll);
        
        SDL_RenderDrawLine(r, tri_cx + (int)p1.x, tri_cy + (int)p1.y, tri_cx + (int)p2.x, tri_cy + (int)p2.y);
        SDL_RenderDrawLine(r, tri_cx + (int)p2.x, tri_cy + (int)p2.y, tri_cx + (int)p3.x, tri_cy + (int)p3.y);
        SDL_RenderDrawLine(r, tri_cx + (int)p3.x, tri_cy + (int)p3.y, tri_cx + (int)p1.x, tri_cy + (int)p1.y);
    }

    /* ---- Aircraft reference symbol (centre, fixed) ---- */
    {
        /* Central L-shaped markers */
        set_color(r, COL_BACKGROUND);
        
        /* Left L marker */
        SDL_Rect left_h = { cx - 55, cy - 2, 33, 5 };
        SDL_Rect left_v = { cx - 27, cy - 2, 5, 20 };
        SDL_RenderFillRect(r, &left_h);
        SDL_RenderFillRect(r, &left_v);
        
        /* Right L marker */
        SDL_Rect right_h = { cx + 22, cy - 2, 33, 5 };
        SDL_Rect right_v = { cx + 22, cy - 2, 5, 20 };
        SDL_RenderFillRect(r, &right_h);
        SDL_RenderFillRect(r, &right_v);

        /* Borders for L markers */
        set_color(r, COL_TEXT_AMBER);
        SDL_RenderDrawRect(r, &left_h);
        SDL_RenderDrawRect(r, &left_v);
        SDL_RenderDrawRect(r, &right_h);
        SDL_RenderDrawRect(r, &right_v);
        
        /* Central dot */
        draw_filled_circle(r, cx, cy, 4);
        
        /* Remove the nose line, just keep the Ls and the dot */
    }

    /* ---- Radio altitude display at bottom of ADI ---- */
    if (f->altitude_agl_ft <= 2500.0f && f->altitude_agl_ft >= -50.0f) {
        char ra_str[16];
        snprintf(ra_str, sizeof(ra_str), "%.0f", (double)f->altitude_agl_ft);
        
        int box_w = 60;
        int box_h = 24;
        int box_x = cx - box_w / 2;
        int box_y = cy + R - box_h - 4; // near the bottom of the ADI circle
        
        SDL_Rect ra_box = { box_x, box_y, box_w, box_h };
        set_color(r, COL_BACKGROUND); // Black background
        SDL_RenderFillRect(r, &ra_box);
        set_color(r, COL_HORIZON_WHITE); // White border
        SDL_RenderDrawRect(r, &ra_box);
        
        /* White text */
        font_draw_scaled_aligned(r, cx, box_y + box_h / 2 + 4, ra_str, 0.65f, FONT_BOLD, FONT_ALIGN_CENTER);
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

    /* ---- Current IAS window (Polygon Box) ---- */
    int box_h = 32;
    set_color(r, COL_BACKGROUND);
    
    /* Draw polygon for current speed box (with a pointer extending right) */
    /*
        p1(tape_l, top) -------- p2(tape_r - 8, top)
        |                              \
        |                               \ p3(tape_r, mid)
        |                               /
        |                              /
        p5(tape_l, bot) -------- p4(tape_r - 8, bot)
    */
    int ptr_x = tape_r;
    int right_edge = tape_r - 8;
    int top_y = tape_mid - box_h / 2;
    int bot_y = tape_mid + box_h / 2;
    
    SDL_Rect ias_box_main = { tape_l, top_y, right_edge - tape_l, box_h };
    SDL_RenderFillRect(r, &ias_box_main);
    
    /* Filled triangle for the pointer part */
    for (int y = top_y; y <= bot_y; y++) {
        int x_end;
        if (y <= tape_mid) {
            float t = (float)(y - top_y) / (float)(tape_mid - top_y);
            x_end = right_edge + (int)(t * 8.0f);
        } else {
            float t = (float)(bot_y - y) / (float)(bot_y - tape_mid);
            x_end = right_edge + (int)(t * 8.0f);
        }
        SDL_RenderDrawLine(r, right_edge, y, x_end, y);
    }
    
    set_color(r, COL_HORIZON_WHITE);
    SDL_RenderDrawLine(r, tape_l, top_y, right_edge, top_y);
    SDL_RenderDrawLine(r, right_edge, top_y, ptr_x, tape_mid);
    SDL_RenderDrawLine(r, ptr_x, tape_mid, right_edge, bot_y);
    SDL_RenderDrawLine(r, right_edge, bot_y, tape_l, bot_y);
    SDL_RenderDrawLine(r, tape_l, bot_y, tape_l, top_y);

    {
        char ias_str[8];
        snprintf(ias_str, sizeof(ias_str), "%d", (int)ias);
        set_color(r, COL_HORIZON_WHITE);
        draw_text_simple(r, tape_l + (right_edge - tape_l) / 2, tape_mid, ias_str, 0.85f);
    }

    /* ---- "SPD" label below box ---- */
    set_color(r, COL_HORIZON_WHITE);
    draw_text_simple(r, tape_l + tape_w / 2,
                     tape_mid + box_h / 2 + 7, "SPD", 0.55f);

    /* ---- Target Speed (Magenta) at top ---- */
    if (f->ap_spd > 0.0f) {
        char tgt_str[8];
        snprintf(tgt_str, sizeof(tgt_str), "%d", (int)f->ap_spd);
        set_color(r, COL_MAGENTA);
        draw_text_simple(r, tape_l + tape_w / 2, tape_y - 12, tgt_str, 0.65f);
    }

    /* ---- Mach number ---- */
    if (f->mach > 0.40f) {
        char mach_str[10];
        snprintf(mach_str, sizeof(mach_str), "M%.3f", (double)f->mach);
        set_color(r, COL_HORIZON_WHITE);
        draw_text_simple(r, tape_l + tape_w / 2,
                         tape_mid + box_h / 2 + 22, mach_str, 0.48f);
    }

    /* ---- AP speed bug (magenta hollow polygon) ---- */
    if (f->ap_spd > 0.0f) {
        float bug_y = (float)tape_mid - (f->ap_spd - ias) * px_kt;
        if (bug_y > (float)tape_y && bug_y < (float)(tape_y + tape_h)) {
            set_color(r, COL_MAGENTA);
            /* Hollow polygon pointing to the right edge */
            int bx = tape_r;
            int by = (int)bug_y;
            SDL_RenderDrawLine(r, bx, by, bx + 6, by - 5);
            SDL_RenderDrawLine(r, bx + 6, by - 5, bx + 12, by - 5);
            SDL_RenderDrawLine(r, bx + 12, by - 5, bx + 12, by + 5);
            SDL_RenderDrawLine(r, bx + 12, by + 5, bx + 6, by + 5);
            SDL_RenderDrawLine(r, bx + 6, by + 5, bx, by);
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

    /* ---- Tick marks (continuous scrolling) ---- */
    float alt_floor = floorf(alt / 100.0f) * 100.0f;
    for (float a = alt_floor - 700.0f; a <= alt + 700.0f; a += 100.0f) {
        float y_pos = (float)tape_mid - (a - alt) * px_ft;
        if (y_pos < (float)tape_y - 3.0f || y_pos > (float)(tape_y + tape_h) + 3.0f)
            continue;

        /* Tick line */
        set_color(r, COL_HORIZON_WHITE);
        int tick_w = ((int)a % 200 == 0) ? tape_w * 2 / 3 : tape_w / 3;
        SDL_RenderDrawLine(r, tape_l, (int)y_pos, tape_l + tick_w, (int)y_pos);

        /* Label (last 3 digits), ONLY if divisible by 200 */
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

    /* ---- Current ALT window (Polygon Box) ---- */
    int box_h = 32;
    set_color(r, COL_BACKGROUND);
    
    /* Draw polygon for current alt box (with a pointer extending left) */
    /*
        p2(tape_l + 8, top) -------- p3(tape_r, top)
             /                              |
            / p1(tape_l, mid)               |
            \                               |
             \                              |
        p5(tape_l + 8, bot) -------- p4(tape_r, bot)
    */
    int ptr_x = tape_l;
    int left_edge = tape_l + 8;
    int top_y = tape_mid - box_h / 2;
    int bot_y = tape_mid + box_h / 2;
    
    SDL_Rect alt_box_main = { left_edge, top_y, tape_r - left_edge, box_h };
    SDL_RenderFillRect(r, &alt_box_main);
    
    /* Filled triangle for the pointer part */
    for (int y = top_y; y <= bot_y; y++) {
        int x_start;
        if (y <= tape_mid) {
            float t = (float)(y - top_y) / (float)(tape_mid - top_y);
            x_start = left_edge - (int)(t * 8.0f);
        } else {
            float t = (float)(bot_y - y) / (float)(bot_y - tape_mid);
            x_start = left_edge - (int)(t * 8.0f);
        }
        SDL_RenderDrawLine(r, x_start, y, left_edge, y);
    }
    
    set_color(r, COL_HORIZON_WHITE);
    SDL_RenderDrawLine(r, left_edge, top_y, tape_r, top_y);
    SDL_RenderDrawLine(r, tape_r, top_y, tape_r, bot_y);
    SDL_RenderDrawLine(r, tape_r, bot_y, left_edge, bot_y);
    SDL_RenderDrawLine(r, left_edge, bot_y, ptr_x, tape_mid);
    SDL_RenderDrawLine(r, ptr_x, tape_mid, left_edge, top_y);

    {
        char alt_str[8];
        snprintf(alt_str, sizeof(alt_str), "%05d", (int)alt);
        set_color(r, COL_HORIZON_WHITE);
        draw_text_simple(r, left_edge + (tape_r - left_edge) / 2, tape_mid, alt_str, 0.85f);
    }

    /* ---- "ALT" label below box ---- */
    set_color(r, COL_HORIZON_WHITE);
    draw_text_simple(r, tape_l + tape_w / 2,
                     tape_mid + box_h / 2 + 7, "ALT", 0.55f);

    /* ---- Target Altitude (Magenta) at top ---- */
    if (f->ap_alt > 0.0f) {
        char tgt_str[8];
        snprintf(tgt_str, sizeof(tgt_str), "%05d", (int)f->ap_alt);
        set_color(r, COL_MAGENTA);
        draw_text_simple(r, tape_l + tape_w / 2, tape_y - 12, tgt_str, 0.65f);
    }

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

    /* ---- AP altitude bug (magenta hollow polygon) ---- */
    if (f->ap_alt > 0.0f) {
        float bug_y = (float)tape_mid - (f->ap_alt - alt) * px_ft;
        if (bug_y > (float)tape_y && bug_y < (float)(tape_y + tape_h)) {
            set_color(r, COL_MAGENTA);
            /* Hollow polygon pointing to the left edge */
            int bx = tape_l;
            int by = (int)bug_y;
            SDL_RenderDrawLine(r, bx, by, bx - 6, by - 5);
            SDL_RenderDrawLine(r, bx - 6, by - 5, bx - 12, by - 5);
            SDL_RenderDrawLine(r, bx - 12, by - 5, bx - 12, by + 5);
            SDL_RenderDrawLine(r, bx - 12, by + 5, bx - 6, by + 5);
            SDL_RenderDrawLine(r, bx - 6, by + 5, bx, by);
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

    float vs_fpm = f->vs_fpm;

    /* ---- Background ---- */
    /* Draw a bridge-like shape. We will use standard lines/rects for now to simulate the "bridge" */
    set_color(r, COL_TAPE_BG);
    SDL_Rect bg = { vsi_l, vsi_y, vsi_w, vsi_h };
    SDL_RenderFillRect(r, &bg);
    
    set_color(r, COL_BOX_BORDER);
    /* Draw a custom border: left line is solid, right line is solid, but we cap top and bottom */
    SDL_RenderDrawLine(r, vsi_l, vsi_y, vsi_r, vsi_y);                 /* Top */
    SDL_RenderDrawLine(r, vsi_l, vsi_y + vsi_h, vsi_r, vsi_y + vsi_h); /* Bottom */
    SDL_RenderDrawLine(r, vsi_l, vsi_y, vsi_l, vsi_y + vsi_h);         /* Left */
    SDL_RenderDrawLine(r, vsi_r, vsi_y, vsi_r, vsi_y + vsi_h);         /* Right */

    /* ---- Zero line ---- */
    set_color(r, COL_HORIZON_WHITE);
    SDL_RenderDrawLine(r, vsi_l, vsi_mid, vsi_r - 2, vsi_mid);

    /* ---- Scale mapping logic ----
     * The scale is non-linear according to prompt:
     * 0 to 2 (x1000) is linear, taking up half the top half of VSI height.
     * 2 to 6 (x1000) is linear, taking up the other half of the top half.
     * So:
     *   0..2000 ft/min maps to 0..25% of total height from center.
     *   2000..6000 ft/min maps to 25%..50% of total height from center.
     * Same for negative values (bottom half).
     */
    float px_per_k_low = ((float)vsi_h / 4.0f) / 2.0f;  /* pixels per 1000 ft/min for 0-2000 range */
    float px_per_k_hi  = ((float)vsi_h / 4.0f) / 4.0f;  /* pixels per 1000 ft/min for 2000-6000 range */

    /* ---- Scale tick marks & labels ---- */
    /* Positive ticks: 0.5, 1.0, 1.5, 2.0, 4.0, 6.0 (x1000 ft/min) */
    /* Negative ticks: -0.5, -1.0, -1.5, -2.0, -4.0, -6.0 */
    static const float vs_ticks_k[] = { 0.5f, 1.0f, 1.5f, 2.0f, 4.0f, 6.0f,
                                       -0.5f, -1.0f, -1.5f, -2.0f, -4.0f, -6.0f };
    for (int i = 0; i < 12; i++) {
        float v_k = vs_ticks_k[i];
        
        /* Calculate y_offset inline */
        float abs_vs_k = fabsf(v_k);
        float offset = 0.0f;
        if (abs_vs_k <= 2.0f) {
            offset = abs_vs_k * px_per_k_low;
        } else {
            offset = (2.0f * px_per_k_low) + ((abs_vs_k - 2.0f) * px_per_k_hi);
        }
        float y_offset = (v_k >= 0.0f) ? -offset : offset;
        
        float y_pos = (float)vsi_mid + y_offset;
        
        if (y_pos < (float)vsi_y + 2.0f || y_pos > (float)(vsi_y + vsi_h) - 2.0f)
            continue;

        /* Minor ticks for 0.5, 1.5; Major for 1.0, 2.0, 4.0, 6.0 */
        int is_major = (fmodf(fabsf(v_k), 1.0f) < 0.1f || fmodf(fabsf(v_k), 2.0f) < 0.1f);
        int tick_w = is_major ? vsi_w * 2 / 3 : vsi_w / 3;
        
        set_color(r, COL_HORIZON_WHITE);
        SDL_RenderDrawLine(r, vsi_l, (int)y_pos, vsi_l + tick_w, (int)y_pos);

        /* Labels for major ticks (1, 2, 4, 6) */
        if (is_major) {
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "%d", (int)fabsf(v_k));
            font_draw_scaled_aligned(r, vsi_r - 2, (int)y_pos,
                                     lbl, 0.42f, FONT_REGULAR, FONT_ALIGN_RIGHT);
        }
    }

    /* ---- Pointer (moving triangle on left) ---- */
    float vs_clamped = clamp_f(vs_fpm, -6000.0f, 6000.0f);
    
    /* Calculate pointer y_offset inline */
    float abs_vs_k_ptr = fabsf(vs_clamped) / 1000.0f;
    float ptr_offset = 0.0f;
    if (abs_vs_k_ptr <= 2.0f) {
        ptr_offset = abs_vs_k_ptr * px_per_k_low;
    } else {
        ptr_offset = (2.0f * px_per_k_low) + ((abs_vs_k_ptr - 2.0f) * px_per_k_hi);
    }
    float ptr_y_offset = (vs_clamped >= 0.0f) ? -ptr_offset : ptr_offset;
    
    int ptr_y = (int)((float)vsi_mid + ptr_y_offset);
    ptr_y = clamp_i(ptr_y, vsi_y + 1, vsi_y + vsi_h - 1);

    /* Pointer is a diagonal line from middle-right to left scale */
    set_color(r, COL_CYAN);
    draw_thick_line(r, vsi_r - 4, vsi_mid, vsi_l, ptr_y, 3);

    /* ---- Digital readout ---- */
    /* Always display readout for debugging purposes */
    char vs_str[16];
    int vs_display = ((int)(fabsf(vs_fpm) / 50.0f)) * 50; /* rounded to nearest 50 */
    snprintf(vs_str, sizeof(vs_str), "%d", vs_display);

    int readout_y = (vs_fpm >= 0) ? (vsi_y - 15) : (vsi_y + vsi_h + 15);
    
    set_color(r, COL_HORIZON_WHITE);
    font_draw_scaled_aligned(r, vsi_l + vsi_w / 2, readout_y,
                             vs_str, 0.55f, FONT_BOLD, FONT_ALIGN_CENTER);
}

/* =========================================================================
 *  Heading tape (bottom) — Half-Compass Rose style
 * ========================================================================= */

static void draw_heading_tape(SDL_Renderer* r, const SDL_Rect* rect,
                               const PFDLayout* lay, const FlightDataValues* f)
{
    int tape_y     = lay->hdg_y;
    int tape_mid_x = rect->x + rect->w / 2;
    
    /* Radius and Center: radius = width / 3.2.
       Center Y is placed so the top of the arc touches the top of the heading area (tape_y).
    */
    float R = (float)rect->w / 3.2f;
    int cx = tape_mid_x;
    int cy = tape_y + (int)R;

    float hdg = f->heading_mag_deg; /* Use magnetic heading as requested */

    /* ---- Background ---- */
    /* Remove grey background and border as requested */
    /*
    set_color(r, COL_TAPE_BG);
    SDL_Rect bg = { rect->x, tape_y, rect->w, tape_h };
    SDL_RenderFillRect(r, &bg);
    set_color(r, COL_BOX_BORDER);
    SDL_RenderDrawRect(r, &bg);
    */

    /* ---- Compass Rose Arc Background ---- */
    /* We can draw a filled arc or circle portion here using points, but simply drawing the outline is fine */
    set_color(r, COL_LINE_GRAY);
    for (int i = -60; i <= 60; i++) {
        float a1 = (float)i * (float)M_PI / 180.0f - (float)M_PI / 2.0f;
        float a2 = (float)(i + 1) * (float)M_PI / 180.0f - (float)M_PI / 2.0f;
        SDL_RenderDrawLine(r,
                           cx + (int)(R * cosf(a1)), cy + (int)(R * sinf(a1)),
                           cx + (int)(R * cosf(a2)), cy + (int)(R * sinf(a2)));
    }

    /* ---- Ticks and Labels ---- */
    /* Draw ticks every 5 degrees, label every 30 degrees.
       The compass rotates. So angle on screen = heading_value - current_heading - 90 deg (to put 0 at top).
       Wait, no: top is current_heading. So if tick is 'h', its angle from top is (h - hdg).
    */
    float hdg_floor = floorf(hdg / 5.0f) * 5.0f;
    for (float h = hdg_floor - 60.0f; h <= hdg + 60.0f; h += 5.0f) {
        float h_val = (float)norm_angle_360((double)h);
        float delta = h - hdg; /* angular distance from top center */
        
        /* Only draw if within visible arc (roughly +/- 45 deg) */
        if (fabsf(delta) > 50.0f) continue;
        
        float angle_rad = (float)DEG2RAD(delta - 90.0f); /* -90 points straight UP */
        
        int abs_h = (int)h_val;
        int is_major = (abs_h % 30 == 0);
        
        /* Make tick lines visibly longer */
        float inner_r = is_major ? R - 20.0f : R - 10.0f;
        
        int x1 = cx + (int)(R * cosf(angle_rad));
        int y1 = cy + (int)(R * sinf(angle_rad));
        int x2 = cx + (int)(inner_r * cosf(angle_rad));
        int y2 = cy + (int)(inner_r * sinf(angle_rad));
        
        set_color(r, COL_HORIZON_WHITE);
        SDL_RenderDrawLine(r, x1, y1, x2, y2);
        
        if (is_major) {
            float text_r = R - 35.0f;
            int tx = cx + (int)(text_r * cosf(angle_rad));
            int ty = cy + (int)(text_r * sinf(angle_rad));
            
            char lbl[16];
            if (abs_h == 0 || abs_h == 360) snprintf(lbl, sizeof(lbl), "N");
            else if (abs_h == 90)  snprintf(lbl, sizeof(lbl), "E");
            else if (abs_h == 180) snprintf(lbl, sizeof(lbl), "S");
            else if (abs_h == 270) snprintf(lbl, sizeof(lbl), "W");
            else                   snprintf(lbl, sizeof(lbl), "%02d", abs_h / 10);
            
            font_draw_scaled_aligned(r, tx, ty, lbl, 0.50f, FONT_REGULAR, FONT_ALIGN_CENTER);
        }
    }

    /* ---- Current Heading Pointer (hollow triangle pointing down OUTSIDE the compass) ---- */
    {
        set_color(r, COL_HORIZON_WHITE);
        /* Base is above tape_y, tip is at tape_y (touching the arc from the outside) */
        int tip_y = tape_y;
        int base_y = tape_y - 15;
        /* Draw hollow triangle pointing DOWN */
        SDL_RenderDrawLine(r, cx, tip_y, cx - 10, base_y);
        SDL_RenderDrawLine(r, cx - 10, base_y, cx + 10, base_y);
        SDL_RenderDrawLine(r, cx + 10, base_y, cx, tip_y);
    }

    /* ---- AP Target Heading Bug (Magenta hollow notch) ---- */
    if (f->ap_hdg >= 0.0f) {
        float delta = (float)norm_angle_360((double)(f->ap_hdg - hdg));
        if (delta > 180.0f) delta -= 360.0f;
        
        if (fabsf(delta) <= 55.0f) { /* only if visible on arc */
            
            /* Define a shape pointing to the outer edge of the arc */
            /* We build the shape points centered at (0, R+4) and rotate */
            Vec2f p1 = vec2f(0.0f, R);
            Vec2f p2 = vec2f(-6.0f, R + 6.0f);
            Vec2f p3 = vec2f(-6.0f, R + 14.0f);
            Vec2f p4 = vec2f(6.0f, R + 14.0f);
            Vec2f p5 = vec2f(6.0f, R + 6.0f);
            
            /* Rotate around the arc */
            float rot = (float)DEG2RAD(delta);
            p1 = vec2f_rotate(p1, rot);
            p2 = vec2f_rotate(p2, rot);
            p3 = vec2f_rotate(p3, rot);
            p4 = vec2f_rotate(p4, rot);
            p5 = vec2f_rotate(p5, rot);
            
            set_color(r, COL_MAGENTA);
            SDL_RenderDrawLine(r, cx + (int)p1.x, cy - (int)p1.y, cx + (int)p2.x, cy - (int)p2.y);
            SDL_RenderDrawLine(r, cx + (int)p2.x, cy - (int)p2.y, cx + (int)p3.x, cy - (int)p3.y);
            SDL_RenderDrawLine(r, cx + (int)p3.x, cy - (int)p3.y, cx + (int)p4.x, cy - (int)p4.y);
            SDL_RenderDrawLine(r, cx + (int)p4.x, cy - (int)p4.y, cx + (int)p5.x, cy - (int)p5.y);
            SDL_RenderDrawLine(r, cx + (int)p5.x, cy - (int)p5.y, cx + (int)p1.x, cy - (int)p1.y);
        }
    }

    /* ---- Heading Information Box (Left side: MAG / True value) ---- */
    {
        /* Target HDG on the left in magenta */
        char tgt_str[8];
        if (f->ap_hdg >= 0.0f) {
            snprintf(tgt_str, sizeof(tgt_str), "%03d", (int)f->ap_hdg);
        } else {
            snprintf(tgt_str, sizeof(tgt_str), "---");
        }
        set_color(r, COL_MAGENTA);
        font_draw_scaled_aligned(r, tape_mid_x - 80, tape_y + 15, tgt_str, 0.55f, FONT_REGULAR, FONT_ALIGN_RIGHT);

        /* MAG label */
        set_color(r, COL_TEXT_GREEN);
        font_draw_scaled_aligned(r, tape_mid_x + 80, tape_y + 15, "MAG", 0.55f, FONT_REGULAR, FONT_ALIGN_LEFT);

        /* Current HDG digital readout in center bottom of the tape */
        char hdg_str[8];
        snprintf(hdg_str, sizeof(hdg_str), "%03d", (int)norm_angle_360(hdg));
        set_color(r, COL_HORIZON_WHITE);
        font_draw_scaled_aligned(r, tape_mid_x, tape_y + 55, hdg_str, 0.70f, FONT_BOLD, FONT_ALIGN_CENTER);
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

    /* ---- Section background ---- */
    /* Remove grey border as requested, but keep background for consistency */
    set_color(r, COL_BACKGROUND);
    SDL_Rect sec = { rect->x, ty, rect->w, th };
    SDL_RenderFillRect(r, &sec);

    for (int eng = 0; eng < 2; eng++) {
        int gx = (eng == 0) ? mid_x - gap / 2 - gauge_w
                            : mid_x + gap / 2;
        int cx = gx + gauge_w / 2 + 10;
        int cy = ty + th / 2;
        float R = (float)th / 2.0f - 8.0f;

        float n1_pct = clamp_f(f->n1_pct[eng], 0.0f, 110.0f);

        /* Engine label */
        char eng_lbl[8];
        snprintf(eng_lbl, sizeof(eng_lbl), "N1 %d", eng + 1);
        set_color(r, COL_TEXT_WHITE);
        font_draw_scaled_aligned(r, gx + 20, ty + 10,
                                 eng_lbl, 0.42f, FONT_REGULAR, FONT_ALIGN_CENTER);

        /* Digital N1% */
        char n1_str[10];
        snprintf(n1_str, sizeof(n1_str), "%.1f", (double)n1_pct);
        if (n1_pct > 100.0f) {
            set_color(r, COL_TEXT_AMBER);
        } else {
            set_color(r, COL_N1_GREEN);
        }
        font_draw_scaled_aligned(r, gx + 20, cy,
                                 n1_str, 0.55f, FONT_BOLD, FONT_ALIGN_CENTER);

        /* ---- Draw Arc (135 deg to 360 deg) ---- */
        set_color(r, COL_LINE_GRAY);
        for (int i = 135; i <= 360; i++) {
            float a1 = (float)i * (float)M_PI / 180.0f;
            float a2 = (float)(i + 1) * (float)M_PI / 180.0f;
            SDL_RenderDrawLine(r,
                               cx + (int)(R * cosf(a1)), cy + (int)(R * sinf(a1)),
                               cx + (int)(R * cosf(a2)), cy + (int)(R * sinf(a2)));
        }

        /* ---- Ticks every 20% (45 deg) and labels ---- */
        set_color(r, COL_HORIZON_WHITE);
        for (int pct = 0; pct <= 100; pct += 20) {
            float angle_deg = 135.0f + ((float)pct / 100.0f) * 225.0f;
            float rad = (float)DEG2RAD(angle_deg);
            float inner_r = R - 6.0f;
            SDL_RenderDrawLine(r,
                               cx + (int)(R * cosf(rad)), cy + (int)(R * sinf(rad)),
                               cx + (int)(inner_r * cosf(rad)), cy + (int)(inner_r * sinf(rad)));
            
            /* Draw tick labels (0, 2, 4, 6, 8, 10 for 0%, 20%, 40% etc.) */
            char tick_lbl[4];
            snprintf(tick_lbl, sizeof(tick_lbl), "%d", pct / 10);
            float text_r = R + 10.0f;
            int t_x = cx + (int)(text_r * cosf(rad));
            int t_y = cy + (int)(text_r * sinf(rad));
            font_draw_scaled_aligned(r, t_x, t_y, tick_lbl, 0.40f, FONT_REGULAR, FONT_ALIGN_CENTER);
        }

        /* ---- Pointer ---- */
        float ptr_angle_deg = 135.0f + (n1_pct / 100.0f) * 225.0f;
        if (ptr_angle_deg > 360.0f) ptr_angle_deg = 360.0f;
        float ptr_rad = (float)DEG2RAD(ptr_angle_deg);
        
        set_color(r, COL_HORIZON_WHITE);
        /* Draw a thicker line pointer from center to edge */
        draw_thick_line(r, cx, cy, cx + (int)(R * cosf(ptr_rad)), cy + (int)(R * sinf(ptr_rad)), 3);
    }
}

/* =========================================================================
 *  Pitch Rate Dial — top right corner (in thrust ref area)
 * ========================================================================= */

static void draw_pitch_rate_dial(SDL_Renderer* r, const SDL_Rect* rect,
                                 const PFDLayout* lay, float pitch_rate)
{
    int ty = lay->thrust_y;
    int th = lay->thrust_h;
    
    /* Place it on the far right of the thrust area */
    int cx = rect->x + rect->w - (rect->w * 10 / 100);
    int cy = ty + th / 2 + 5;
    float R = (float)th / 2.0f - 8.0f;
    
    /* Clamp pitch rate to [-60, 60] */
    float pr = clamp_f(pitch_rate, -60.0f, 60.0f);
    
    /* Label */
    set_color(r, COL_TEXT_WHITE);
    font_draw_scaled_aligned(r, cx, ty + 10, "PITCH RATE", 0.40f, FONT_REGULAR, FONT_ALIGN_CENTER);

    /* Draw arc from 45 to 315 degrees (0 is at 180 deg / left) */
    set_color(r, COL_LINE_GRAY);
    for (int i = 45; i <= 315; i++) {
        float a1 = (float)i * (float)M_PI / 180.0f;
        float a2 = (float)(i + 1) * (float)M_PI / 180.0f;
        SDL_RenderDrawLine(r,
                           cx + (int)(R * cosf(a1)), cy + (int)(R * sinf(a1)),
                           cx + (int)(R * cosf(a2)), cy + (int)(R * sinf(a2)));
    }

    /* Draw ticks */
    set_color(r, COL_HORIZON_WHITE);
    for (int rate = -60; rate <= 60; rate += 20) {
        float angle_deg = 180.0f + ((float)rate / 60.0f) * 135.0f;
        float rad = (float)DEG2RAD(angle_deg);
        float inner_r = R - 5.0f;
        SDL_RenderDrawLine(r,
                           cx + (int)(R * cosf(rad)), cy + (int)(R * sinf(rad)),
                           cx + (int)(inner_r * cosf(rad)), cy + (int)(inner_r * sinf(rad)));
                           
        if (rate == 0 || rate == 60 || rate == -60) {
            char lbl[8];
            snprintf(lbl, sizeof(lbl), "%d", rate);
            float text_r = R + 10.0f;
            int t_x = cx + (int)(text_r * cosf(rad));
            int t_y = cy + (int)(text_r * sinf(rad));
            font_draw_scaled_aligned(r, t_x, t_y, lbl, 0.35f, FONT_REGULAR, FONT_ALIGN_CENTER);
        }
    }

    /* Pointer */
    float ptr_angle_deg = 180.0f + (pr / 60.0f) * 135.0f;
    float ptr_rad = (float)DEG2RAD(ptr_angle_deg);
    set_color(r, COL_CYAN);
    draw_thick_line(r, cx, cy, cx + (int)(R * cosf(ptr_rad)), cy + (int)(R * sinf(ptr_rad)), 2);
    
    /* Digital readout */
    char pr_str[10];
    snprintf(pr_str, sizeof(pr_str), "%.1f", (double)pr);
    set_color(r, COL_HORIZON_WHITE);
    font_draw_scaled_aligned(r, cx, cy + (int)R + 10, pr_str, 0.45f, FONT_BOLD, FONT_ALIGN_CENTER);
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

    /* Column layout — 3 equal columns */
    int col_w = rect->w / 3;

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
            set_color(r, COL_BOX_BORDER);
            SDL_RenderDrawRect(r, &box);
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
            set_color(r, COL_BOX_BORDER);
            SDL_RenderDrawRect(r, &box);
            set_color(r, COL_MAGENTA);
            font_draw_scaled_aligned(r, cx, mid_y, "CWSR", 0.55f,
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

        if (f->ap_engaged) {
            set_color(r, COL_FMA_BOX_BG);
            SDL_Rect box = { bx, by, bw, bh };
            SDL_RenderFillRect(r, &box);
            set_color(r, COL_BOX_BORDER);
            SDL_RenderDrawRect(r, &box);
            set_color(r, COL_MAGENTA);
            font_draw_scaled_aligned(r, cx, mid_y, "CWSP", 0.55f,
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
    
    /* Robust VSI calculation from altitude to avoid X-Plane version/unit mapping issues */
    if (p->last_alt_valid && dt > 0.001) {
        float inst_vs = (p->fd.altitude_ft - p->last_alt_for_vsi) / (float)dt * 60.0f;
        /* Smooth the calculated VSI slightly more to avoid jitter */
        float vs_alpha = (float)(1.0 - exp(-dt * 2.0));
        p->smooth_vs = exp_smooth(p->smooth_vs, inst_vs, vs_alpha);
    }
    p->last_alt_for_vsi = p->fd.altitude_ft;
    p->last_alt_valid = 1;

    /* Robust Pitch Rate calculation from pitch_deg */
    if (p->last_pitch_valid && dt > 0.001) {
        float inst_pitch_rate = (p->fd.pitch_deg - p->last_pitch_for_rate) / (float)dt;
        /* Smooth the calculated pitch rate */
        float pr_alpha = (float)(1.0 - exp(-dt * 4.0));
        p->smooth_pitch_rate = exp_smooth(p->smooth_pitch_rate, inst_pitch_rate, pr_alpha);
    }
    p->last_pitch_for_rate = p->fd.pitch_deg;
    p->last_pitch_valid = 1;

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

    /* ---- Draw Attitude Indicator FIRST ---- */
    draw_attitude_indicator(renderer, &lay, &f);

    /* ---- Draw remaining PFD components (top → bottom, left → right) ---- */
    draw_fma(renderer, rect, &lay, &f);
    draw_thrust_ref(renderer, rect, &lay, &f);
    draw_pitch_rate_dial(renderer, rect, &lay, p->smooth_pitch_rate);
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
