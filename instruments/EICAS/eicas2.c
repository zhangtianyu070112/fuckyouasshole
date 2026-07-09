/**
 * @file    eicas2.c
 * @brief   EICAS2 implementation.
 */

#include "eicas2.h"
#include "app.h"
#include "data/flight_data.h"
#include "utils/math_util.h"
#include "utils/font_manager.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* === Colors === */
#define COL_BG         0x10, 0x10, 0x18, 255
#define COL_TAPE_BG    0x20, 0x20, 0x28, 255
#define COL_WHITE      0xFF, 0xFF, 0xFF, 255
#define COL_GREEN      0x00, 0xFF, 0x40, 255
#define COL_AMBER      0xFF, 0xC0, 0x00, 255
#define COL_RED        0xFF, 0x30, 0x30, 255
#define COL_CYAN       0x00, 0xFF, 0xFF, 255
#define COL_GRAY       0x60, 0x60, 0x68, 255
#define COL_DARK_GRAY  0x30, 0x30, 0x38, 255

typedef struct {
    float smooth_n2[2];
    float smooth_ff[2];
    float smooth_oil_press[2];
    float smooth_oil_temp[2];
    float smooth_oil_qty[2];
    float smooth_vib[2];
    FlightDataValues fd;         /* Latest flight data snapshot */
} EICAS2Data;

static void set_col(SDL_Renderer* r, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    SDL_SetRenderDrawColor(r, R, G, B, A);
}

/* Engine arc gauge — unified N2 dial */
static void draw_engine_arc(SDL_Renderer* r, int cx, int cy, int radius,
                            float value, float max_val, float redline,
                            const char* label, const char* unit)
{
    float arc_start = 180.0f;
    float arc_end   = 360.0f;
    float sweep     = arc_end - arc_start;  /* 180° */

    if (value < 0.0f) value = 0.0f;
    float pct = (max_val > 0.0f) ? (value / max_val) : 0.0f;
    if (pct > 1.2f) pct = 1.2f;

    set_col(r, COL_GRAY);
    int arc_pts = 100;
    int track_w = 8;
    for (int i = 0; i < arc_pts; i++) {
        float t1 = (float)i / (float)arc_pts;
        float t2 = (float)(i + 1) / (float)arc_pts;
        float a1 = (float)DEG2RAD((double)(arc_start + sweep * t1));
        float a2 = (float)DEG2RAD((double)(arc_start + sweep * t2));

        for (int w = radius; w < radius + track_w; w++) {
            SDL_RenderDrawLine(r,
                cx + (int)((float)w * cosf(a1)), cy + (int)((float)w * sinf(a1)),
                cx + (int)((float)w * cosf(a2)), cy + (int)((float)w * sinf(a2)));
        }
    }

    {
        int num_ticks = 11;
        for (int t = 0; t < num_ticks; t++) {
            float tick_pct = (float)t / (float)(num_ticks - 1);
            float deg = arc_start + sweep * tick_pct;
            float rad = (float)DEG2RAD((double)deg);
            float c = cosf(rad), s = sinf(rad);
            int is_major = (t % 2 == 0);
            float inner_r = (float)(radius + track_w);
            float outer_r = (float)(radius + track_w + (is_major ? 10 : 5));

            set_col(r, COL_WHITE);
            SDL_RenderDrawLine(r,
                cx + (int)(inner_r * c), cy + (int)(inner_r * s),
                cx + (int)(outer_r * c), cy + (int)(outer_r * s));

            if (is_major) {
                float lbl_r = outer_r + 10.0f;
                char lbl[8];
                int tick_val = (int)(max_val * tick_pct / 10.0f + 0.5f) * 10;
                snprintf(lbl, sizeof(lbl), "%d", tick_val);
                set_col(r, COL_WHITE);
                draw_text_simple(r,
                    cx + (int)(lbl_r * c), cy + (int)(lbl_r * s), lbl, 0.32f);
            }
        }
    }

    {
        float needle_pct = (pct < 1.2f) ? pct : 1.2f;
        if (needle_pct > 1.0f) needle_pct = 1.0f;
        float deg = arc_start + sweep * needle_pct;
        float rad = (float)DEG2RAD((double)deg);
        float c = cosf(rad), s = sinf(rad);
        float needle_len = (float)(radius - 4);

        if (value >= redline) {
            set_col(r, COL_RED);
        } else {
            set_col(r, COL_WHITE);
        }

        SDL_RenderDrawLine(r,
            cx - (int)(3.0f * s), cy + (int)(3.0f * c),
            cx + (int)(needle_len * c), cy + (int)(needle_len * s));
        SDL_RenderDrawLine(r,
            cx + (int)(3.0f * s), cy - (int)(3.0f * c),
            cx + (int)(needle_len * c), cy + (int)(needle_len * s));
    }

    set_col(r, COL_DARK_GRAY);
    draw_filled_circle(r, cx, cy, 5);
    set_col(r, COL_WHITE);
    draw_filled_circle(r, cx, cy, 2);

    {
        char val_str[16];
        snprintf(val_str, sizeof(val_str), "%.1f", (double)value);
        if (value >= redline) {
            set_col(r, COL_RED);
        } else {
            set_col(r, COL_WHITE);
        }
        draw_text_simple(r, cx, cy + radius / 5, val_str, 0.9f);
    }

    {
        set_col(r, COL_CYAN);
        draw_text_simple(r, cx, cy - radius - 4, label, 0.55f);
    }

    {
        set_col(r, COL_GRAY);
        draw_text_simple(r, cx, cy + radius / 5 + 18, unit, 0.38f);
    }
}

/* Vertical scale line gauge */
static void draw_vertical_scale(SDL_Renderer* r, int cx, int cy, int height,
                                float value, float max_val, const char* label)
{
    int half_h = height / 2;
    int top_y = cy - half_h;
    int bot_y = cy + half_h;

    /* Draw label */
    set_col(r, COL_CYAN);
    draw_text_simple(r, cx, top_y - 12, label, 0.45f);

    /* Draw scale line */
    set_col(r, COL_WHITE);
    SDL_RenderDrawLine(r, cx, top_y, cx, bot_y);
    
    /* Top, mid, bottom ticks */
    SDL_RenderDrawLine(r, cx - 4, top_y, cx, top_y);
    SDL_RenderDrawLine(r, cx - 4, cy, cx, cy);
    SDL_RenderDrawLine(r, cx - 4, bot_y, cx, bot_y);

    /* Draw pointer (red line) */
    float pct = (max_val > 0.0f) ? (value / max_val) : 0.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    
    int ptr_y = bot_y - (int)(pct * (float)height);
    
    set_col(r, COL_RED);
    SDL_RenderDrawLine(r, cx - 10, ptr_y - 1, cx + 10, ptr_y - 1);
    SDL_RenderDrawLine(r, cx - 10, ptr_y, cx + 10, ptr_y);
    SDL_RenderDrawLine(r, cx - 10, ptr_y + 1, cx + 10, ptr_y + 1);

    /* Value text */
    char val_str[16];
    snprintf(val_str, sizeof(val_str), "%.0f", (double)value);
    set_col(r, COL_WHITE);
    draw_text_simple(r, cx, bot_y + 12, val_str, 0.5f);
}

static void eicas2_on_init(Instrument* self, App* app)
{
    (void)app;
    EICAS2Data* d = (EICAS2Data*)self->private_data;
    d->smooth_n2[0] = 0.0f; d->smooth_n2[1] = 0.0f;
    d->smooth_ff[0] = 0.0f; d->smooth_ff[1] = 0.0f;
    d->smooth_oil_press[0] = 0.0f; d->smooth_oil_press[1] = 0.0f;
    d->smooth_oil_temp[0] = 0.0f; d->smooth_oil_temp[1] = 0.0f;
    d->smooth_oil_qty[0] = 0.0f; d->smooth_oil_qty[1] = 0.0f;
    d->smooth_vib[0] = 0.0f; d->smooth_vib[1] = 0.0f;

    LOG_DEBUG("EICAS2 initialized");
}

static void eicas2_on_update(Instrument* self, const FlightData* fd, float dt)
{
    (void)dt;
    EICAS2Data* d = (EICAS2Data*)self->private_data;
    if (!fd) return;

    flight_data_snapshot((FlightData*)fd, &d->fd);

    float alpha = 0.12f;
    for (int i = 0; i < 2; i++) {
        d->smooth_n2[i]  = exp_smooth(d->smooth_n2[i],  d->fd.n2_pct[i], alpha);
        d->smooth_ff[i]  = exp_smooth(d->smooth_ff[i],  d->fd.fuel_flow_pph[i], alpha);
        d->smooth_oil_press[i] = exp_smooth(d->smooth_oil_press[i], d->fd.oil_press_psi[i], alpha);
        d->smooth_oil_temp[i]  = exp_smooth(d->smooth_oil_temp[i], d->fd.oil_temp_c[i], alpha);
        
        /* Pseudo values for oil qty and vib since they are not in flight data */
        float pseudo_oil_qty = 85.0f + d->fd.n1_pct[i] * 0.05f;
        d->smooth_oil_qty[i]   = exp_smooth(d->smooth_oil_qty[i], pseudo_oil_qty, alpha);
        
        float pseudo_vib = d->fd.n1_pct[i] * 0.015f + 0.2f;
        d->smooth_vib[i]       = exp_smooth(d->smooth_vib[i], pseudo_vib, alpha);
    }
}

static void eicas2_on_render(Instrument* self, SDL_Renderer* renderer)
{
    EICAS2Data* d = (EICAS2Data*)self->private_data;
    const SDL_Rect* rect = &self->rect;

    set_col(renderer, COL_BG);
    SDL_RenderFillRect(renderer, rect);

    set_col(renderer, COL_TAPE_BG);
    SDL_Rect title = { rect->x, rect->y, rect->w, 18 };
    SDL_RenderFillRect(renderer, &title);
    set_col(renderer, COL_WHITE);
    draw_text_simple(renderer, rect->x + rect->w / 2, rect->y + 9, "EICAS 2", 0.7f);

    int title_h = 20;
    int content_y = rect->y + title_h + 10;

    int left_col_x  = rect->x + rect->w * 25 / 100;
    int right_col_x = rect->x + rect->w * 75 / 100;

    /* N2 Arcs */
    int arc_radius = rect->w * 15 / 100;
    if (arc_radius > 95)  arc_radius = 95;
    if (arc_radius < 50)  arc_radius = 50;

    int n2_cy = content_y + arc_radius + 10;
    
    draw_engine_arc(renderer, left_col_x, n2_cy, arc_radius,
                    d->smooth_n2[0], 100.0f, 100.0f, "N2", "%");
    draw_engine_arc(renderer, right_col_x, n2_cy, arc_radius,
                    d->smooth_n2[1], 100.0f, 100.0f, "N2", "%");

    /* Vertical spacing */
    int y_step = 60;
    int cur_y = n2_cy + arc_radius + 30;

    /* FF */
    set_col(renderer, COL_CYAN);
    font_draw_scaled_aligned(renderer, left_col_x, cur_y - 12, "FF", 0.45f, FONT_REGULAR, FONT_ALIGN_CENTER);
    font_draw_scaled_aligned(renderer, right_col_x, cur_y - 12, "FF", 0.45f, FONT_REGULAR, FONT_ALIGN_CENTER);

    char val_l[16], val_r[16];
    snprintf(val_l, sizeof(val_l), "%.0f", (double)d->smooth_ff[0]);
    snprintf(val_r, sizeof(val_r), "%.0f", (double)d->smooth_ff[1]);
    
    set_col(renderer, COL_WHITE);
    font_draw_scaled_aligned(renderer, left_col_x, cur_y + 4, val_l, 0.6f, FONT_REGULAR, FONT_ALIGN_CENTER);
    font_draw_scaled_aligned(renderer, right_col_x, cur_y + 4, val_r, 0.6f, FONT_REGULAR, FONT_ALIGN_CENTER);

    cur_y += y_step;

    /* OIL PRESS */
    draw_vertical_scale(renderer, left_col_x, cur_y + 20, 50, d->smooth_oil_press[0], 100.0f, "OIL PRESS");
    draw_vertical_scale(renderer, right_col_x, cur_y + 20, 50, d->smooth_oil_press[1], 100.0f, "OIL PRESS");
    
    cur_y += y_step + 30;

    /* OIL TEMP */
    draw_vertical_scale(renderer, left_col_x, cur_y + 20, 50, d->smooth_oil_temp[0], 200.0f, "OIL TEMP");
    draw_vertical_scale(renderer, right_col_x, cur_y + 20, 50, d->smooth_oil_temp[1], 200.0f, "OIL TEMP");

    cur_y += y_step + 30;

    /* OIL QTY */
    set_col(renderer, COL_CYAN);
    font_draw_scaled_aligned(renderer, left_col_x, cur_y - 12, "OIL QTY", 0.45f, FONT_REGULAR, FONT_ALIGN_CENTER);
    font_draw_scaled_aligned(renderer, right_col_x, cur_y - 12, "OIL QTY", 0.45f, FONT_REGULAR, FONT_ALIGN_CENTER);

    snprintf(val_l, sizeof(val_l), "%.1f", (double)d->smooth_oil_qty[0]);
    snprintf(val_r, sizeof(val_r), "%.1f", (double)d->smooth_oil_qty[1]);
    
    set_col(renderer, COL_WHITE);
    font_draw_scaled_aligned(renderer, left_col_x, cur_y + 4, val_l, 0.6f, FONT_REGULAR, FONT_ALIGN_CENTER);
    font_draw_scaled_aligned(renderer, right_col_x, cur_y + 4, val_r, 0.6f, FONT_REGULAR, FONT_ALIGN_CENTER);

    cur_y += y_step;

    /* VIB */
    set_col(renderer, COL_CYAN);
    font_draw_scaled_aligned(renderer, left_col_x, cur_y - 12, "VIB", 0.45f, FONT_REGULAR, FONT_ALIGN_CENTER);
    font_draw_scaled_aligned(renderer, right_col_x, cur_y - 12, "VIB", 0.45f, FONT_REGULAR, FONT_ALIGN_CENTER);

    snprintf(val_l, sizeof(val_l), "%.1f", (double)d->smooth_vib[0]);
    snprintf(val_r, sizeof(val_r), "%.1f", (double)d->smooth_vib[1]);
    
    set_col(renderer, COL_WHITE);
    font_draw_scaled_aligned(renderer, left_col_x, cur_y + 4, val_l, 0.6f, FONT_REGULAR, FONT_ALIGN_CENTER);
    font_draw_scaled_aligned(renderer, right_col_x, cur_y + 4, val_r, 0.6f, FONT_REGULAR, FONT_ALIGN_CENTER);

    set_col(renderer, COL_GRAY);
    SDL_RenderDrawRect(renderer, rect);
}

static int eicas2_on_event(Instrument* self, const SDL_Event* ev)
{
    (void)self; (void)ev;
    return 0;
}

static void eicas2_on_destroy(Instrument* self)
{
    if (self && self->private_data) {
        free(self->private_data);
        self->private_data = NULL;
    }
}

Instrument* eicas2_create(void)
{
    Instrument* inst = calloc(1, sizeof(Instrument));
    if (!inst) return NULL;

    EICAS2Data* data = calloc(1, sizeof(EICAS2Data));
    if (!data) { free(inst); return NULL; }

    inst->name         = "EICAS2";
    inst->on_init      = eicas2_on_init;
    inst->on_update    = eicas2_on_update;
    inst->on_render    = eicas2_on_render;
    inst->on_event     = eicas2_on_event;
    inst->on_destroy   = eicas2_on_destroy;
    inst->private_data = data;

    return inst;
}
