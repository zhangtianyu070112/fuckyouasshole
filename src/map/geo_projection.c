/**
 * @file    geo_projection.c
 * @brief   Web Mercator (EPSG:3857) projection implementation.
 *
 * Key formulas:
 *   World X (normalized 0..1) = (lon + 180) / 360
 *   World Y (normalized 0..1) = (1 − ln(tan(lat_rad) + sec(lat_rad)) / π) / 2
 *   Pixel at zoom z = world_coord × TILE_SIZE × 2^z
 */

#include "geo_projection.h"
#include <math.h>

/* =========================================================================
 *  Public API
 * ========================================================================= */

void geo_to_tile(double lat, double lon, int zoom,
                 double* tile_x, double* tile_y)
{
    double lat_rad = GEO_DEG2RAD(lat);
    double scale   = (double)(1 << zoom);  /* 2^zoom */

    /* World X: normalized 0..1 */
    double wx = (lon + 180.0) / 360.0;

    /* World Y: Mercator projection, normalized 0..1 */
    double wy = 0.5 - (log(tan(lat_rad) + 1.0 / cos(lat_rad))) / (2.0 * M_PI);

    *tile_x = wx * scale;
    *tile_y = wy * scale;
}

void geo_to_screen(double lat, double lon,
                   double center_lat, double center_lon,
                   int zoom, int map_w, int map_h,
                   int* px, int* py)
{
    /* Convert both the point and the center to world pixel coordinates */
    double pt_tx, pt_ty;
    double cx_tx, cy_ty;

    geo_to_tile(lat, lon, zoom, &pt_tx, &pt_ty);
    geo_to_tile(center_lat, center_lon, zoom, &cx_tx, &cy_ty);

    /* Tile coordinates → pixel coordinates (1 tile = GEO_TILE_SIZE pixels) */
    double pt_px = pt_tx * (double)GEO_TILE_SIZE;
    double pt_py = pt_ty * (double)GEO_TILE_SIZE;
    double cx_px = cx_tx * (double)GEO_TILE_SIZE;
    double cy_py = cy_ty * (double)GEO_TILE_SIZE;

    /* Offset from center → screen pixel */
    *px = (int)(pt_px - cx_px + (double)map_w / 2.0);
    *py = (int)(pt_py - cy_py + (double)map_h / 2.0);
}

double geo_haversine_nm(double lat1, double lon1,
                        double lat2, double lon2)
{
    static const double R_NM = 3440.065;  /* Earth radius in nautical miles */

    double dlat = GEO_DEG2RAD(lat2 - lat1);
    double dlon = GEO_DEG2RAD(lon2 - lon1);
    double a = sin(dlat / 2.0) * sin(dlat / 2.0)
             + cos(GEO_DEG2RAD(lat1)) * cos(GEO_DEG2RAD(lat2))
             * sin(dlon / 2.0) * sin(dlon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return R_NM * c;
}

