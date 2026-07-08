/**
 * @file    nd_data.h
 * @brief   Navigation Display (ND) specific data structure.
 *
 * Encapsulates the X-Plane DREF data required for the ND instrument,
 * with validity flags to avoid rendering stale or invalid data.
 *
 * DREF indices (per project spec):
 *   Index 0 — sim/flightmodel/position/latitude       (float, degrees)
 *   Index 1 — sim/flightmodel/position/longitude      (float, degrees)
 *   Index 2 — sim/flightmodel/position/mag_psi        (float, degrees)
 *   Index 3 — sim/flightmodel/position/true_airspeed  (float, m/s)
 *   Index 4 — sim/flightmodel/position/groundspeed    (float, m/s)
 *
 * TAS and GS come from X-Plane in m/s; this module converts to knots.
 */

#ifndef ND_DATA_H
#define ND_DATA_H

#include <stdint.h>

/* =========================================================================
 *  ND-specific navigation data
 * ========================================================================= */

typedef struct NDNavData {
    /* --- Raw DREF values --- */
    double  lat_deg;            /* Latitude  (-90.0 .. +90.0)              */
    double  lon_deg;            /* Longitude (-180.0 .. +180.0)            */
    float   mag_heading_deg;    /* Magnetic heading (0..360)               */
    float   true_airspeed_ms;   /* True airspeed (m/s from X-Plane)        */
    float   ground_speed_ms;    /* Ground speed   (m/s from X-Plane)       */

    /* --- Derived / converted values --- */
    float   tas_kts;            /* True airspeed converted to knots        */
    float   gs_kts;             /* Ground speed converted to knots         */

    /* --- Validity flags (bit-field) --- */
    uint8_t lat_valid    : 1;   /* Index 0 received and non-trivial        */
    uint8_t lon_valid    : 1;   /* Index 1 received and non-trivial        */
    uint8_t heading_valid: 1;   /* Index 2 received                        */
    uint8_t tas_valid    : 1;   /* Index 3 received and > 0                */
    uint8_t gs_valid     : 1;   /* Index 4 received and >= 0               */
    uint8_t _reserved    : 3;   /* Padding                                */
} NDNavData;

/* =========================================================================
 *  Helpers
 * ========================================================================= */

/** Zero-initialize an NDNavData struct. */
static inline void nd_nav_data_init(NDNavData* nd)
{
    if (!nd) return;
    nd->lat_deg         = 0.0;
    nd->lon_deg         = 0.0;
    nd->mag_heading_deg = 0.0f;
    nd->true_airspeed_ms = 0.0f;
    nd->ground_speed_ms  = 0.0f;
    nd->tas_kts          = 0.0f;
    nd->gs_kts           = 0.0f;
    nd->lat_valid        = 0;
    nd->lon_valid        = 0;
    nd->heading_valid    = 0;
    nd->tas_valid        = 0;
    nd->gs_valid         = 0;
}

/** Check if all five DREF fields are valid. */
static inline int nd_nav_data_all_valid(const NDNavData* nd)
{
    if (!nd) return 0;
    return nd->lat_valid && nd->lon_valid && nd->heading_valid
           && nd->tas_valid && nd->gs_valid;
}

/** Check if at least position is valid (minimum for ND rendering). */
static inline int nd_nav_data_position_valid(const NDNavData* nd)
{
    if (!nd) return 0;
    return nd->lat_valid && nd->lon_valid;
}

/**
 * @brief Convert raw DREF values to derived fields.
 *
 * Call after updating the raw DREF fields. Sets TAS/GS in knots from m/s.
 * Conversion factor: 1 m/s = 1.94384 knots.
 */
static inline void nd_nav_data_convert(NDNavData* nd)
{
    if (!nd) return;
#define MS_TO_KTS 1.94384f
    if (nd->tas_valid) nd->tas_kts = nd->true_airspeed_ms * MS_TO_KTS;
    if (nd->gs_valid)  nd->gs_kts  = nd->ground_speed_ms  * MS_TO_KTS;
#undef MS_TO_KTS
}

#endif /* ND_DATA_H */
