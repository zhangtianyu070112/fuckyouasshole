/**
 * @file    math_util.h
 * @brief   Math utilities for instrument rendering.
 *
 * Provides:
 *   - Angle normalization and conversion (degrees ↔ radians)
 *   - Linear interpolation and smoothing
 *   - 2D coordinate transformations (rotation, translation)
 *   - Clamping and range mapping
 */

#ifndef MATH_UTIL_H
#define MATH_UTIL_H

#include <math.h>
#include <float.h>

/* =========================================================================
 *  Constants
 * ========================================================================= */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG2RAD(d)  ((d) * M_PI / 180.0)
#define RAD2DEG(r)  ((r) * 180.0 / M_PI)

/* =========================================================================
 *  Angle utilities
 * ========================================================================= */

/** Normalize angle to [0, 360) */
static inline double norm_angle_360(double deg)
{
    deg = fmod(deg, 360.0);
    if (deg < 0.0) deg += 360.0;
    return deg;
}

/** Normalize angle to [-180, 180) */
static inline double norm_angle_180(double deg)
{
    deg = fmod(deg + 180.0, 360.0);
    if (deg < 0.0) deg += 180.0;
    else           deg -= 180.0;
    return deg;
}

/** Shortest signed angular difference from a to b (°), range (-180, 180] */
static inline double angle_diff_deg(double a, double b)
{
    return norm_angle_180(b - a);
}

/* =========================================================================
 *  Clamping & mapping
 * ========================================================================= */

/** Clamp x to [lo, hi] */
static inline double clamp_d(double x, double lo, double hi)
{
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

static inline float clamp_f(float x, float lo, float hi)
{
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

static inline int clamp_i(int x, int lo, int hi)
{
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

/** Map value from [in_lo, in_hi] to [out_lo, out_hi] linearly */
static inline double map_range_d(double val,
                                  double in_lo, double in_hi,
                                  double out_lo, double out_hi)
{
    if (in_lo == in_hi) return out_lo;
    double t = (val - in_lo) / (in_hi - in_lo);
    return out_lo + t * (out_hi - out_lo);
}

static inline float map_range_f(float val,
                                 float in_lo, float in_hi,
                                 float out_lo, float out_hi)
{
    return (float)map_range_d((double)val, (double)in_lo, (double)in_hi,
                              (double)out_lo, (double)out_hi);
}

/* =========================================================================
 *  Interpolation
 * ========================================================================= */

/** Linear interpolation */
static inline float lerp_f(float a, float b, float t)
{
    return a + t * (b - a);
}

/** Smooth exponential moving average: new = old * (1-a) + target * a */
static inline float exp_smooth(float current, float target, float alpha)
{
    return current + alpha * (target - current);
}

/** Angle-aware smooth: handles 0°/360° wrap correctly */
static inline float exp_smooth_angle(float current, float target, float alpha)
{
    float diff = target - current;
    if (diff > 180.0f)       diff -= 360.0f;
    else if (diff < -180.0f) diff += 360.0f;
    float result = current + alpha * diff;
    if (result < 0.0f)       result += 360.0f;
    if (result >= 360.0f)    result -= 360.0f;
    return result;
}

/* =========================================================================
 *  2D Vector operations
 * ========================================================================= */

typedef struct { float x; float y; } Vec2f;

static inline Vec2f vec2f(float x, float y) { Vec2f v = {x, y}; return v; }
static inline Vec2f vec2f_add(Vec2f a, Vec2f b) { return vec2f(a.x + b.x, a.y + b.y); }
static inline Vec2f vec2f_sub(Vec2f a, Vec2f b) { return vec2f(a.x - b.x, a.y - b.y); }
static inline Vec2f vec2f_scale(Vec2f v, float s) { return vec2f(v.x * s, v.y * s); }

/** Rotate a 2D vector by angle (degrees CCW, standard math convention) */
static inline Vec2f vec2f_rotate(Vec2f v, float deg)
{
    float rad = (float)DEG2RAD(deg);
    float c = cosf(rad);
    float s = sinf(rad);
    return vec2f(v.x * c - v.y * s, v.x * s + v.y * c);
}

/* =========================================================================
 *  SDL drawing helpers
 * ========================================================================= */

#include <SDL2/SDL.h>

/** Draw a filled circle */
void draw_filled_circle(SDL_Renderer* r, int cx, int cy, int radius);

/** Draw a thick line (by drawing multiple offset lines) */
void draw_thick_line(SDL_Renderer* r, int x1, int y1, int x2, int y2, int thickness);

/** Draw text centered at (x, y) using B612 Regular font.
 *  Color comes from current render draw color. */
void draw_text_simple(SDL_Renderer* r, int x, int y, const char* text, float scale);

/** Draw text left-aligned at (x, y) using B612 Regular font.
 *  (x, y) is the left-center point of the first character. */
void draw_text_left(SDL_Renderer* r, int x, int y, const char* text, float scale);

/** Draw a horizontal line centered at (cx, cy) with given length */
static inline void draw_h_line(SDL_Renderer* r, int cx, int cy, int len)
{
    SDL_RenderDrawLine(r, cx - len / 2, cy, cx + len / 2, cy);
}

/** Draw a vertical line centered at (cx, cy) with given length */
static inline void draw_v_line(SDL_Renderer* r, int cx, int cy, int len)
{
    SDL_RenderDrawLine(r, cx, cy - len / 2, cx, cy + len / 2);
}

#endif /* MATH_UTIL_H */
