/**
 * @file    geo_projection.h
 * @brief   Web Mercator (EPSG:3857) coordinate transformation for map rendering.
 *
 * Converts between geographic coordinates (lat/lon) and screen pixel coordinates
 * for overlaying flight trajectory on static map tiles from 高德 Static Map API.
 *
 * All formulas follow the standard Web Mercator projection used by Google Maps,
 * OpenStreetMap, and 高德地图.
 */

#ifndef GEO_PROJECTION_H
#define GEO_PROJECTION_H

/* =========================================================================
 *  Constants
 * ========================================================================= */

/** Standard map tile size in pixels (256×256). */
#define GEO_TILE_SIZE        256

/** Earth circumference at equator in meters. */
#define GEO_EARTH_CIRCUM     40075016.686

/** Degrees-to-radians conversion. */
#define GEO_DEG2RAD(d)       ((d) * 0.017453292519943295)

/* =========================================================================
 *  API
 * ========================================================================= */

/**
 * @brief Convert lat/lon to fractional tile coordinates at a given zoom level.
 *
 * @param lat    Latitude in decimal degrees (-90..+90).
 * @param lon    Longitude in decimal degrees (-180..+180).
 * @param zoom   Zoom level (1–18 for 高德).
 * @param tile_x [out] Fractional tile X coordinate.
 * @param tile_y [out] Fractional tile Y coordinate.
 */
void geo_to_tile(double lat, double lon, int zoom,
                 double* tile_x, double* tile_y);

/**
 * @brief Convert lat/lon to screen pixel coordinates relative to a map center.
 *
 * The map is a static image centered at (center_lon, center_lat) at a given
 * zoom level with pixel dimensions (map_w × map_h).
 *
 * Origin (0,0) is the top-left corner of the map image.
 *
 * @param lat          Point latitude.
 * @param lon          Point longitude.
 * @param center_lat   Map center latitude.
 * @param center_lon   Map center longitude.
 * @param zoom         Map zoom level.
 * @param map_w        Map image width in pixels.
 * @param map_h        Map image height in pixels.
 * @param px           [out] Screen pixel X coordinate.
 * @param py           [out] Screen pixel Y coordinate.
 */
void geo_to_screen(double lat, double lon,
                   double center_lat, double center_lon,
                   int zoom, int map_w, int map_h,
                   int* px, int* py);

/**
 * @brief Compute the Haversine distance between two points in nautical miles.
 *
 * @param lat1, lon1  First point (decimal degrees).
 * @param lat2, lon2  Second point (decimal degrees).
 * @return Distance in nautical miles.
 */
double geo_haversine_nm(double lat1, double lon1,
                        double lat2, double lon2);

#endif /* GEO_PROJECTION_H */
