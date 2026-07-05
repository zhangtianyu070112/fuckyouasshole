/**
 * @file    math_util.c
 * @brief   Math utility implementations (SDL drawing helpers).
 */

#include "math_util.h"
#include "font_manager.h"
#include <stdlib.h>

/* --- Draw filled circle (midpoint algorithm variant) ------------------- */

void draw_filled_circle(SDL_Renderer* r, int cx, int cy, int radius)
{
    if (radius <= 0) return;

    /* Draw horizontal lines from top to bottom */
    int x = radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        /* Draw horizontal lines for each octant pair */
        SDL_RenderDrawLine(r, cx - x, cy - y, cx + x, cy - y);
        SDL_RenderDrawLine(r, cx - y, cy - x, cx + y, cy - x);
        SDL_RenderDrawLine(r, cx - x, cy + y, cx + x, cy + y);
        SDL_RenderDrawLine(r, cx - y, cy + x, cx + y, cy + x);

        y++;
        err += 1 + 2 * y;
        if (2 * (err - x) + 1 > 0) {
            x--;
            err += 1 - 2 * x;
        }
    }
}

/* --- Draw thick line (draw parallel offset lines) ----------------------- */

void draw_thick_line(SDL_Renderer* r, int x1, int y1, int x2, int y2, int thickness)
{
    if (thickness <= 1) {
        SDL_RenderDrawLine(r, x1, y1, x2, y2);
        return;
    }

    /* Compute perpendicular direction */
    float dx = (float)(x2 - x1);
    float dy = (float)(y2 - y1);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) {
        /* Degenerate: just draw a rectangle or circle */
        draw_filled_circle(r, x1, y1, thickness / 2);
        return;
    }
    float px = -dy / len;
    float py =  dx / len;

    float half = (float)(thickness - 1) * 0.5f;

    /* Draw N parallel lines offset by px/py */
    for (int i = 0; i < thickness; i++) {
        float offset = -half + (float)i;
        SDL_RenderDrawLine(r,
                           (int)((float)x1 + px * offset + 0.5f),
                           (int)((float)y1 + py * offset + 0.5f),
                           (int)((float)x2 + px * offset + 0.5f),
                           (int)((float)y2 + py * offset + 0.5f));
    }
}

void draw_text_simple(SDL_Renderer* r, int x, int y, const char* text, float scale)
{
    if (!text || !text[0]) return;
    font_draw_scaled(r, x, y, text, scale, FONT_REGULAR);
}

void draw_text_left(SDL_Renderer* r, int x, int y, const char* text, float scale)
{
    if (!text || !text[0]) return;
    font_draw_scaled_aligned(r, x, y, text, scale, FONT_REGULAR, FONT_ALIGN_LEFT);
}
