/**
 * @file    trajectory_render.h
 * @brief   SDL2 flight trajectory overlay rendering.
 *
 * Renders on top of the map background:
 *   - Planned route (dashed yellow polyline)
 *   - Flown path (solid blue polyline)
 *   - Waypoint markers (dots + text labels)
 *   - Departure/arrival markers (green/red circles)
 *   - Aircraft icon (triangle rotated by heading)
 *
 * All coordinates are projected from lat/lon to screen pixels using the
 * geo_to_screen() function from geo_projection.h.
 */

#ifndef TRAJECTORY_RENDER_H
#define TRAJECTORY_RENDER_H

#include <SDL2/SDL.h>

/* Forward declarations */
typedef struct FlightDataValues FlightDataValues;
typedef struct FMCState         FMCState;

/* =========================================================================
 *  API
 * ========================================================================= */

/**
 * @brief Render the full trajectory overlay on the map.
 *
 * @param r            SDL renderer.
 * @param center_lat   Map center latitude (for geo→screen projection).
 * @param center_lon   Map center longitude.
 * @param zoom         Map zoom level.
 * @param map_w        Map display width in pixels.
 * @param map_h        Map display height in pixels.
 * @param map_ox       Map area left edge in window coords (for offset).
 * @param map_oy       Map area top edge in window coords.
 * @param fd           Current flight data snapshot.
 * @param fmc          FMC state (may be NULL → skip FMC route drawing).
 * @param track        GPS breadcrumb trail (array of lat/lon pairs).
 * @param track_count  Number of entries in track array.
 */
void trajectory_render(SDL_Renderer* r,
                       double center_lat, double center_lon,
                       int zoom, int map_w, int map_h,
                       int map_ox, int map_oy,
                       const FlightDataValues* fd,
                       const FMCState* fmc,
                       const double* track_lats,
                       const double* track_lons,
                       int track_count);

/**
 * @brief Draw the aircraft icon (filled triangle) at the given screen position.
 *
 * @param r         SDL renderer.
 * @param cx, cy    Screen center of the aircraft.
 * @param heading   Heading in degrees true (0 = north, clockwise).
 * @param scale     Scale factor (1.0 = normal).
 */
void trajectory_draw_aircraft(SDL_Renderer* r, int cx, int cy,
                              float heading, float scale);

/**
 * @brief Draw a dashed line between two screen points.
 *
 * @param r             SDL renderer.
 * @param x1, y1, x2, y2  Line endpoints.
 * @param dash_len      Length of dash segments.
 * @param gap_len       Length of gaps between dashes.
 */
void trajectory_draw_dashed_line(SDL_Renderer* r,
                                 int x1, int y1, int x2, int y2,
                                 int dash_len, int gap_len);

#endif /* TRAJECTORY_RENDER_H */
