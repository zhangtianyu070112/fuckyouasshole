/**
 * @file    eicas.c
 * @brief   EICAS implementation.
 *
 * Layout (dual-engine):
 *   ┌──────────────────────────────────────┐
 *   │  EICAS       [Alerts area]           │
 *   ├────────┬────────┬────────────────────┤
 *   │  N1    │  N1    │                    │
 *   │ gauge  │ gauge  │   Status / data    │
 *   │ (L)    │ (R)    │                    │
 *   ├────────┼────────┤                    │
 *   │  EGT   │  EGT   │                    │
 *   │  bar   │  bar   │                    │
 *   ├────────┼────────┼────────────────────┤
 *   │  Fuel flow / total fuel              │
 *   └──────────────────────────────────────┘
 */

#include "eicas.h"
#include "app.h"
#include "data/flight_data.h"
#include "utils/math_util.h"
#include "utils/font_manager.h"
#include "utils/logger.h"
#include "ds/linked_list.h"

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
#define COL_GAUGE_INNER 0x18, 0x18, 0x22, 255

/* =========================================================================
 *  Alert definitions & event log types
 * ========================================================================= */

typedef struct {
    const char* label;
    int  active;
    int  is_red;      /* 1 = red warning, 0 = amber caution */
} AlertDef;

typedef struct {
    char    label[16];
    int     active;       /* 1 = became active, 0 = cleared */
    Uint32  timestamp;
} AlertEvent;

/** Evaluate all CAS alert conditions from flight data. Returns count. */
static int eval_alerts(const FlightDataValues* f, AlertDef alerts_out[12])
{
    int count = 0;
    alerts_out[count++] = (AlertDef){
        "GEAR", f->gear_deployed == 0 && f->altitude_agl_ft < 500.0f
                && f->altitude_agl_ft > 10.0f, 1 };
    alerts_out[count++] = (AlertDef){
        "OVHT", f->egt_c[0] > 950.0f || f->egt_c[1] > 950.0f, 1 };
    alerts_out[count++] = (AlertDef){
        "ENG SPD", f->n1_pct[0] > 105.0f || f->n1_pct[1] > 105.0f, 1 };
    alerts_out[count++] = (AlertDef){
        "FUEL", f->fuel_total_lbs < 2000.0f && f->fuel_total_lbs > 0.0f, 0 };
    alerts_out[count++] = (AlertDef){
        "OIL", f->oil_press_psi[0] < 20.0f || f->oil_press_psi[1] < 20.0f, 0 };
    alerts_out[count++] = (AlertDef){
        "FLAP", f->flap_ratio > 0.3f && f->ias_kts > 250.0f, 0 };
    alerts_out[count++] = (AlertDef){ "ELEC",
        f->elec_bus_volts > 1.0f && f->elec_bus_volts < 24.0f, 0 };
    alerts_out[count++] = (AlertDef){ "HYD",
        (f->hyd_press_psi[0] > 1.0f && f->hyd_press_psi[0] < 1000.0f)
     || (f->hyd_press_psi[1] > 1.0f && f->hyd_press_psi[1] < 1000.0f), 0 };
    alerts_out[count++] = (AlertDef){ "DOOR", 0, 0 };
    alerts_out[count++] = (AlertDef){ "ANTI-ICE",
        f->anti_ice_wing || f->anti_ice_eng[0], 0 };
    alerts_out[count++] = (AlertDef){ "APU", f->apu_running, 0 };
    alerts_out[count++] = (AlertDef){ "BAT",
        f->elec_bus_volts > 1.0f && f->elec_bus_volts < 22.0f, 0 };
    return count;
}

typedef struct {
    float smooth_n1[2];
    float smooth_egt[2];
    float smooth_ff[2];
    FlightDataValues fd;         /* Latest flight data snapshot */

    /* Alert event log (linked list of AlertEvent) */
    LinkedList* alert_log;
    AlertDef    prev_alerts[12]; /* Previous frame alert states */
    int         prev_alert_count;
    int         prev_initialized;
} EICASData;

static void set_col(SDL_Renderer* r, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    SDL_SetRenderDrawColor(r, R, G, B, A);
}

/* =========================================================================
 *  Engine arc gauge — unified N1 / EGT dial
 *
 *  Draws a ~180° bottom-half arc (9:00 → 3:00 through 6:00)
 *  with tick marks, needle, and digital readout. No colored zones.
 * ========================================================================= */

static void draw_engine_arc(SDL_Renderer* r, int cx, int cy, int radius,
                            float value, float max_val, float redline,
                            const char* label, const char* unit,
                            int is_egt)
{
    /* Arc geometry: 180° sweep from 180° (left) to 360° (right) through bottom */
    float arc_start = 180.0f;
    float arc_end   = 360.0f;
    float sweep     = arc_end - arc_start;  /* 180° */

    /* Clamp value */
    if (value < 0.0f) value = 0.0f;
    float pct = (max_val > 0.0f) ? (value / max_val) : 0.0f;
    if (pct > 1.2f) pct = 1.2f;

    /* --- Arc track (simple gray band, no color zones) --- */
    set_col(r, COL_GRAY);
    int arc_pts = 100;
    int track_w = 8;  /* arc band thickness */
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

    /* --- Tick marks (every ~10% of sweep) --- */
    {
        int num_ticks = 11;  /* 0%, 10%, ... 100% */
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

            /* Major tick labels (every 20%) */
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

    /* --- Needle --- */
    {
        float needle_pct = (pct < 1.2f) ? pct : 1.2f;
        if (needle_pct > 1.0f) needle_pct = 1.0f;
        float deg = arc_start + sweep * needle_pct;
        float rad = (float)DEG2RAD((double)deg);
        float c = cosf(rad), s = sinf(rad);
        float needle_len = (float)(radius - 4);

        set_col(r, (value >= redline) ? COL_RED : COL_WHITE);

        /* Thick needle: two parallel lines */
        SDL_RenderDrawLine(r,
            cx - (int)(3.0f * s), cy + (int)(3.0f * c),
            cx + (int)(needle_len * c), cy + (int)(needle_len * s));
        SDL_RenderDrawLine(r,
            cx + (int)(3.0f * s), cy - (int)(3.0f * c),
            cx + (int)(needle_len * c), cy + (int)(needle_len * s));
    }

    /* Center hub */
    set_col(r, COL_DARK_GRAY);
    draw_filled_circle(r, cx, cy, 5);
    set_col(r, COL_WHITE);
    draw_filled_circle(r, cx, cy, 2);

    /* --- Digital readout (center, inside arc) --- */
    {
        char val_str[16];
        if (is_egt)
            snprintf(val_str, sizeof(val_str), "%.0f", (double)value);
        else
            snprintf(val_str, sizeof(val_str), "%.1f", (double)value);
        set_col(r, (value >= redline) ? COL_RED : COL_WHITE);
        draw_text_simple(r, cx, cy + radius / 5, val_str, 0.9f);
    }

    /* --- Label (above arc) --- */
    {
        set_col(r, COL_CYAN);
        draw_text_simple(r, cx, cy - radius - 4, label, 0.55f);
    }

    /* --- Unit (below readout) --- */
    {
        set_col(r, COL_GRAY);
        draw_text_simple(r, cx, cy + radius / 5 + 18, unit, 0.38f);
    }
}

/* =========================================================================
 *  ENG1 / ENG2 parameter tables (top-right)
 * ========================================================================= */

static void draw_eng_tables(SDL_Renderer* r, int x, int y, int w,
                            const FlightDataValues* f)
{
    int col_w = w / 2;  /* Two columns: ENG1 | ENG2 */
    int row_h = 19;
    int x1 = x + 4;            /* ENG1 column center */
    int x2 = x + col_w + 4;    /* ENG2 column center */
    int x1r = x + col_w - 4;   /* ENG1 value right-align */
    int x2r = x + w - 4;       /* ENG2 value right-align */

    /* Title row */
    set_col(r, COL_WHITE);
    draw_text_simple(r, x1 + col_w / 2 - 8, y, "ENG1", 0.55f);
    draw_text_simple(r, x2 + col_w / 2 - 8, y, "ENG2", 0.55f);
    y += row_h;

    /* Separator */
    set_col(r, COL_DARK_GRAY);
    SDL_RenderDrawLine(r, x, y - 8, x + w, y - 8);

    /* Row labels (left edge of table) */
    const char* row_labels[] = { "N1 %", "EGT", "N2 %" };
    float vals[3][2] = {
        { f->n1_pct[0],     f->n1_pct[1]     },
        { f->egt_c[0],      f->egt_c[1]      },
        { f->n2_pct[0],     f->n2_pct[1]     }
    };
    const char* formats[] = { "%.1f", "%.0f", "%.1f" };

    for (int row = 0; row < 3; row++) {
        /* Row label */
        set_col(r, COL_CYAN);
        font_draw_scaled_aligned(r, x + 6, y + 7, row_labels[row],
                                 0.45f, FONT_REGULAR, FONT_ALIGN_LEFT);

        /* ENG1 value */
        char v1[12], v2[12];
        snprintf(v1, sizeof(v1), formats[row], (double)vals[row][0]);
        snprintf(v2, sizeof(v2), formats[row], (double)vals[row][1]);
        set_col(r, COL_WHITE);
        font_draw_scaled_aligned(r, x1r, y + 7, v1,
                                 0.5f, FONT_REGULAR, FONT_ALIGN_RIGHT);
        font_draw_scaled_aligned(r, x2r, y + 7, v2,
                                 0.5f, FONT_REGULAR, FONT_ALIGN_RIGHT);

        y += row_h;
    }
}

/* =========================================================================
 *  Fuel QTY box (bottom-right)
 * ========================================================================= */

static void draw_fuel_qty_box(SDL_Renderer* r, int x, int y, int w, int h,
                              float fuel_lbs)
{
    /* Box border */
    set_col(r, COL_GRAY);
    SDL_Rect box = { x, y, w, h };
    SDL_RenderDrawRect(r, &box);
    SDL_Rect box_inner = { x + 2, y + 2, w - 4, h - 4 };
    set_col(r, COL_TAPE_BG);
    SDL_RenderFillRect(r, &box_inner);

    int cy = y + 10;

    /* Title */
    set_col(r, COL_WHITE);
    draw_text_simple(r, x + w / 2, cy, "FUEL QTY", 0.55f);
    cy += 16;

    /* Unit */
    set_col(r, COL_CYAN);
    draw_text_simple(r, x + w / 2, cy, "LBS x1000", 0.45f);
    cy += 18;

    /* Value — large */
    float fuel_1000 = fuel_lbs / 1000.0f;
    char val_str[12];
    snprintf(val_str, sizeof(val_str), "%.1f", (double)fuel_1000);
    set_col(r, COL_GREEN);
    draw_text_simple(r, x + w / 2, cy, val_str, 0.85f);
    cy += 20;

    /* TOTAL label */
    set_col(r, COL_CYAN);
    draw_text_simple(r, x + w / 2, cy, "TOTAL", 0.5f);
}

/* =========================================================================
 *  vtable
 * ========================================================================= */

static void eicas_on_init(Instrument* self, App* app)
{
    (void)app;
    EICASData* d = (EICASData*)self->private_data;
    d->smooth_n1[0] = 0.0f; d->smooth_n1[1] = 0.0f;
    d->smooth_egt[0] = 0.0f; d->smooth_egt[1] = 0.0f;
    d->smooth_ff[0] = 0.0f; d->smooth_ff[1] = 0.0f;

    /* Create alert event log (linked list) */
    d->alert_log = ll_create();
    memset(d->prev_alerts, 0, sizeof(d->prev_alerts));
    d->prev_alert_count = 0;
    d->prev_initialized = 0;

    LOG_DEBUG("EICAS initialized (alert log: %s)",
              d->alert_log ? "OK" : "FAIL");
}

static void eicas_on_update(Instrument* self, const FlightData* fd, float dt)
{
    (void)dt;
    EICASData* d = (EICASData*)self->private_data;
    if (!fd) return;

    /* Take full snapshot for render */
    flight_data_snapshot((FlightData*)fd, &d->fd);

    float alpha = 0.12f;
    for (int i = 0; i < 2; i++) {
        d->smooth_n1[i]  = exp_smooth(d->smooth_n1[i],  d->fd.n1_pct[i], alpha);
        d->smooth_egt[i] = exp_smooth(d->smooth_egt[i], d->fd.egt_c[i], alpha);
        d->smooth_ff[i]  = exp_smooth(d->smooth_ff[i],  d->fd.fuel_flow_pph[i], alpha);
    }

    /* --- Evaluate alerts & log state transitions --- */
    {
        AlertDef current[12];
        int count = eval_alerts(&d->fd, current);

        if (d->prev_initialized) {
            for (int i = 0; i < count && i < d->prev_alert_count; i++) {
                if (current[i].active != d->prev_alerts[i].active) {
                    /* State changed — log event */
                    AlertEvent* evt = calloc(1, sizeof(AlertEvent));
                    if (evt) {
                        strncpy(evt->label, current[i].label, sizeof(evt->label) - 1);
                        evt->active    = current[i].active;
                        evt->timestamp = SDL_GetTicks();
                        ll_push_back(d->alert_log, evt);
                        /* Cap at 100 events */
                        while (ll_size(d->alert_log) > 100) {
                            AlertEvent* old = (AlertEvent*)ll_pop_front(d->alert_log);
                            free(old);
                        }
                        LOG_DEBUG("EICAS alert: %s → %s",
                                  current[i].label, current[i].active ? "ON" : "OFF");
                    }
                }
            }
        }

        /* Save current state for next frame */
        memcpy(d->prev_alerts, current, sizeof(current));
        d->prev_alert_count = count;
        d->prev_initialized = 1;
    }
}

static void eicas_on_render(Instrument* self, SDL_Renderer* renderer)
{
    EICASData* d = (EICASData*)self->private_data;
    const SDL_Rect* rect = &self->rect;

    /* Background */
    set_col(renderer, COL_BG);
    SDL_RenderFillRect(renderer, rect);

    /* Title strip */
    set_col(renderer, COL_TAPE_BG);
    SDL_Rect title = { rect->x, rect->y, rect->w, 18 };
    SDL_RenderFillRect(renderer, &title);
    set_col(renderer, COL_WHITE);
    draw_text_simple(renderer, rect->x + rect->w / 2, rect->y + 9, "EICAS", 0.7f);

    /* Master Warning / Caution — right side of title bar */
    {
        const FlightDataValues* f = &d->fd;
        if (f->master_warning) {
            int blink = ((SDL_GetTicks() / 500) % 2);
            if (blink) {
                set_col(renderer, COL_RED);
                font_draw_scaled_aligned(renderer, rect->x + rect->w - 8, rect->y + 9,
                                         "WARNING", 0.55f, FONT_BOLD, FONT_ALIGN_RIGHT);
            }
        } else if (f->master_caution) {
            set_col(renderer, COL_AMBER);
            font_draw_scaled_aligned(renderer, rect->x + rect->w - 8, rect->y + 9,
                                     "CAUTION", 0.55f, FONT_BOLD, FONT_ALIGN_RIGHT);
        }
    }

    /* =====================================================================
     *  NEW LAYOUT (from reference):
     *    Left 70% — engine arcs:  N1(L) N1(R) / EGT(L) EGT(R) / FF
     *    Right 30% — ENG1/ENG2 tables + Fuel QTY box
     *    Bottom — CAS alerts + event log
     * ===================================================================== */

    const FlightDataValues* f = &d->fd;

    /* --- TAT display (top right corner of EICAS screen) --- */
    {
        char tat_str[24];
        snprintf(tat_str, sizeof(tat_str), "TAT %+.0f°C", (double)f->tat_c);
        set_col(renderer, COL_WHITE);
        font_draw_scaled_aligned(renderer, rect->x + rect->w - 20, rect->y + 25,
                                 tat_str, 0.6f, FONT_REGULAR, FONT_ALIGN_RIGHT);
    }

    /* --- Layout dimensions --- */
    int title_h   = 20;
    int engine_top = rect->y + title_h + 16;

    /* Arc sizing: make arcs as large as the column width allows */
    int left_col_x  = rect->x + rect->w * 3 / 100;
    int right_col_x = rect->x + rect->w * 33 / 100;
    int col_w       = rect->w * 28 / 100;

    int table_x     = rect->x + rect->w * 64 / 100;
    int table_w     = rect->w * 33 / 100;

    int arc_radius = col_w * 42 / 100;  /* ~42% of column width */
    if (arc_radius > 95)  arc_radius = 95;
    if (arc_radius < 50)  arc_radius = 50;

    int arc_gap = arc_radius / 4;  /* Tight gap between N1 and EGT arcs */

    int n1_cy   = engine_top + arc_radius + 12;
    int egt_cy  = n1_cy + arc_radius * 2 + arc_gap;
    int ff_y    = egt_cy + arc_radius + 12;  /* FF right below EGT arc */

    int eng_cx_l = left_col_x  + col_w / 2;
    int eng_cx_r = right_col_x + col_w / 2;

    /* Total engine height for CAS positioning */
    int engine_h = ff_y + 28 - engine_top;

    /* === N1 arcs (left & right) === */
    draw_engine_arc(renderer, eng_cx_l, n1_cy, arc_radius,
                    d->smooth_n1[0], 100.0f, 100.0f, "N1", "%", 0);
    draw_engine_arc(renderer, eng_cx_r, n1_cy, arc_radius,
                    d->smooth_n1[1], 100.0f, 100.0f, "N1", "%", 0);

    /* === EGT arcs (left & right) === */
    draw_engine_arc(renderer, eng_cx_l, egt_cy, arc_radius,
                    d->smooth_egt[0], 1000.0f, 950.0f, "EGT", "°C", 1);
    draw_engine_arc(renderer, eng_cx_r, egt_cy, arc_radius,
                    d->smooth_egt[1], 1000.0f, 950.0f, "EGT", "°C", 1);

    /* === FF (Fuel Flow) readouts === */
    {
        char ff_l[16], ff_r[16];
        snprintf(ff_l, sizeof(ff_l), "FF %.0f", (double)d->smooth_ff[0]);
        snprintf(ff_r, sizeof(ff_r), "FF %.0f", (double)d->smooth_ff[1]);
        set_col(renderer, COL_WHITE);
        font_draw_scaled_aligned(renderer, eng_cx_l, ff_y, ff_l,
                                 0.5f, FONT_REGULAR, FONT_ALIGN_CENTER);
        font_draw_scaled_aligned(renderer, eng_cx_r, ff_y, ff_r,
                                 0.5f, FONT_REGULAR, FONT_ALIGN_CENTER);
        /* "PPH" label below FF */
        set_col(renderer, COL_GRAY);
        font_draw_scaled_aligned(renderer, eng_cx_l, ff_y + 14, "PPH",
                                 0.35f, FONT_REGULAR, FONT_ALIGN_CENTER);
        font_draw_scaled_aligned(renderer, eng_cx_r, ff_y + 14, "PPH",
                                 0.35f, FONT_REGULAR, FONT_ALIGN_CENTER);
    }

    /* === ENG1 / ENG2 parameter tables (right side, top) === */
    {
        int tab_y = engine_top + 12;
        int tab_h = 100;
        draw_eng_tables(renderer, table_x, tab_y, table_w, f);

        /* Thin separator below tables */
        set_col(renderer, COL_DARK_GRAY);
        SDL_RenderDrawLine(renderer, table_x, tab_y + tab_h,
                           table_x + table_w, tab_y + tab_h);
    }

    /* === Fuel QTY box (right side, below tables) === */
    {
        int fuel_x = table_x + table_w / 4;    /* Center the fuel box */
        int fuel_w = table_w / 2;
        int fuel_y = engine_top + 120;
        int fuel_h = rect->y + rect->h - fuel_y - 8;
        if (fuel_h < 80) fuel_h = 80;
        draw_fuel_qty_box(renderer, fuel_x, fuel_y, fuel_w, fuel_h,
                          f->fuel_total_lbs);
    }

    /* Border */
    set_col(renderer, COL_GRAY);
    SDL_RenderDrawRect(renderer, rect);
}

static int eicas_on_event(Instrument* self, const SDL_Event* ev)
{
    (void)self; (void)ev;
    return 0;
}

static void eicas_on_destroy(Instrument* self)
{
    if (self && self->private_data) {
        EICASData* d = (EICASData*)self->private_data;
        if (d->alert_log) {
            ll_destroy(d->alert_log, 1);  /* free AlertEvent nodes */
            d->alert_log = NULL;
        }
        free(self->private_data);
        self->private_data = NULL;
    }
}

/* =========================================================================
 *  Constructor
 * ========================================================================= */

Instrument* eicas_create(void)
{
    Instrument* inst = calloc(1, sizeof(Instrument));
    if (!inst) return NULL;

    EICASData* data = calloc(1, sizeof(EICASData));
    if (!data) { free(inst); return NULL; }

    inst->name         = "EICAS";
    inst->on_init      = eicas_on_init;
    inst->on_update    = eicas_on_update;
    inst->on_render    = eicas_on_render;
    inst->on_event     = eicas_on_event;
    inst->on_destroy   = eicas_on_destroy;
    inst->private_data = data;

    return inst;
}
