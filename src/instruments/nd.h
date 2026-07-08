/**
 * @file    nd.h
 * @brief   Navigation Display (ND) instrument.
 *
 * Displays a compass rose with:
 *   - Heading (rotating compass card)
 *   - Range rings with distance labels
 *   - Aircraft symbol at center
 *   - Wind vector
 *   - Active waypoint / track line
 *   - Selected heading bug
 *   - Mode (ROSE / ARC / PLAN) and range selector
 *   - Course deviation indicator (CDI) / course marker
 *   - Nearby navigation entries from spatial hash
 */

#ifndef ND_H
#define ND_H

#include "instrument.h"
#include "../data/navdata.h"
#include "../data/flight_data.h"
#include "../ds/spatial_hash.h"

/* =========================================================================
 *  ND data structure
 * ========================================================================= */

/**
 * @brief ND private data — owns all display state and nav data caches.
 *
 * Design principle: "全面覆盖核心数据、明确数据有效性、适配X-Plane数据格式"
 * All position/speed/heading fields have validity flags to prevent rendering
 * invalid data from X-Plane.
 */
typedef struct {
    /* --- Display state --- */
    int    mode;                /* 0=VOR, 1=MAP, 2=APP, 3=PLN */
    int    range_nm;            /* Current range setting (10/20/40/80/160/320) */
    float  smooth_hdg;          /* Smoothed heading for display animation */

    /* --- Data sources --- */
    FMCState*       fmc;        /* FMC shared state (flight plan, nav db) */
    FlightDataValues fd;        /* Latest flight data snapshot */

    /* --- Spatial hash (grid-based nav lookup) --- */
    SpatialHash*    spatial_hash;

    /* --- Display toggles --- */
    int    show_waypoints;      /* 1 = draw nav waypoints from spatial hash */
    int    show_airports;       /* 1 = draw nav airports from spatial hash */
    int    show_route;          /* 1 = draw active flight plan route */
    int    show_navaids;        /* 1 = draw VOR/NDB stations */
    int    show_course;         /* 1 = draw course deviation marker */

    /* --- Ground track computation (from GPS position delta) --- */
    double prev_lat, prev_lon;
    float  track_true_deg;
    float  track_mag_deg;
    int    track_valid;

    /* --- Nearby nav entries cache (refreshed each frame) --- */
    NavSpatialEntry* nearby_entries[512];
    int    nearby_count;

    /* --- Active course for CDI --- */
    float  crs_selector_deg;   /* Selected course on NAV1 OBS */
    float  crs_deviation;      /* CDI deflection (−1..+1 from NAV1) */

} NDData;

/* =========================================================================
 *  Constructor
 * ========================================================================= */

Instrument* nd_create(void);

#endif /* ND_H */
