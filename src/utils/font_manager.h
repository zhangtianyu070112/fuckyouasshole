/**
 * @file    font_manager.h
 * @brief   TTF font loading and texture-cached text rendering.
 *
 * Uses a global singleton pattern — call font_system_init() once after
 * SDL_ttf is initialized, and font_system_shutdown() before SDL_Quit().
 *
 * All text rendering reads the current render draw color automatically,
 * so existing set_color() calls before draw_text_simple() will work.
 */

#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdint.h>

/* =========================================================================
 *  Font identifiers
 * ========================================================================= */

typedef enum {
    FONT_REGULAR = 0,    /* B612-Regular.ttf  — cockpit labels & values   */
    FONT_BOLD,           /* B612-Bold.ttf     — titles & warnings          */
    FONT_MONO,           /* B612Mono-Regular.ttf — FMC/CDU scratchpad      */
    FONT_MONO_BOLD,      /* B612Mono-Bold.ttf — FMC/CDU titles             */
    FONT_COUNT
} FontID;

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

/**
 * @brief Initialize the global font system.
 * @param font_dir  Path to directory containing .ttf files.
 * @return 0 on success, -1 on failure (logs warnings for missing files).
 */
int  font_system_init(const char* font_dir);

/**
 * @brief Shutdown the font system — frees all TTF_Font objects
 *        and cached textures.
 */
void font_system_shutdown(void);

/* =========================================================================
 *  Text alignment
 * ========================================================================= */

typedef enum {
    FONT_ALIGN_CENTER = 0,
    FONT_ALIGN_LEFT,
    FONT_ALIGN_RIGHT
} FontAlign;

/* =========================================================================
 *  Text drawing
 * ========================================================================= */

/**
 * @brief Draw text at (x, y) with specified alignment.
 *
 * Color is read from the renderer's current draw color — call
 * SDL_SetRenderDrawColor() before this function.
 *
 * @param r         SDL renderer.
 * @param x, y      Anchor point (interpreted per align).
 * @param text      UTF-8 string to render (must be null-terminated).
 * @param pt_size   Font size in points.
 * @param font_id   Which font face to use.
 * @param align     FONT_ALIGN_LEFT / _CENTER / _RIGHT.
 */
void font_draw_aligned(SDL_Renderer* r, int x, int y, const char* text,
                       int pt_size, FontID font_id, FontAlign align);

/** @brief Convenience: font_draw_aligned with FONT_ALIGN_CENTER */
void font_draw(SDL_Renderer* r, int x, int y, const char* text,
               int pt_size, FontID font_id);

/**
 * @brief Convenience: scale-based with explicit alignment.
 *
 * Scale → pt_size mapping:
 *   0.40 →  9pt    0.55 → 12pt    0.70 → 15pt    0.85 → 19pt
 *   0.45 → 10pt    0.60 → 13pt    0.75 → 16pt    0.90 → 20pt
 *   0.50 → 11pt    0.65 → 14pt    0.80 → 18pt    1.00 → 22pt
 */
void font_draw_scaled_aligned(SDL_Renderer* r, int x, int y, const char* text,
                              float scale, FontID font_id, FontAlign align);

/** @brief Convenience: font_draw_scaled_aligned with FONT_ALIGN_CENTER */
void font_draw_scaled(SDL_Renderer* r, int x, int y, const char* text,
                      float scale, FontID font_id);

#endif /* FONT_MANAGER_H */
