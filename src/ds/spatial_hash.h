/**
 * @file    spatial_hash.h
 * @brief   Grid-based spatial hash table for navigation data.
 *
 * Partitions the Earth's surface by latitude/longitude grids and stores
 * navigation entries (waypoints, airports, VORs, NDBs, towers) in a
 * hash table keyed by grid cell. Query uses heading-prioritized
 * neighbor-cell search with configurable result limit.
 *
 * Design:
 *   - 1° latitude bands (0..180, covering -90° to +90°)
 *   - Dynamic longitude cell width = 1.0 / cos(lat_rad) → ~60 NM per cell
 *   - 64-bit packed key: (uint32_t)lat_band << 32 | (uint32_t)lon_sector
 *   - Separate chaining per grid cell via NavSpatialEntry::next
 *   - All entries malloc'd on heap; destroy walks all chains
 *
 * Capacity: tested with 200k+ waypoints + 37k navaids.
 */

#ifndef SPATIAL_HASH_H
#define SPATIAL_HASH_H

#include "hash_table.h"
#include <stdint.h>

/* =========================================================================
 *  Types
 * ========================================================================= */

/** Navigation entry type. */
typedef enum {
    NAV_WAYPOINT = 0,   /* Enroute waypoint / fix */
    NAV_AIRPORT,         /* Airport reference point */
    NAV_TOWER,           /* ATC tower */
    NAV_VOR,             /* VOR/DME station */
    NAV_NDB              /* NDB station */
} NavSpatialType;

struct NavSpatialEntry;  /* forward declaration (tag only) */

/** Query result with pre-computed distance and bearing.
 *  Avoids duplicate Haversine calls in ND rendering. */
typedef struct {
    struct NavSpatialEntry* entry; /* pointer into spatial hash (read-only) */
    float            dist_nm;     /* distance from query position (NM) */
    float            bearing_deg; /* bearing from query position (°) */
} NavQueryResult;

/** A single navigation entry stored in the spatial hash grid. */
typedef struct NavSpatialEntry {
    char              ident[16];        /* Identifier (e.g. "LADIX", "ZBAA") */
    NavSpatialType    type;
    double            lat_deg;
    double            lon_deg;
    float             elevation_ft;
    float             freq_khz;         /* Radio frequency in kHz (VOR/NDB) or 0 */
    float             mag_var_deg;      /* Magnetic variation */
    char              name[64];         /* Full name (airports, navaids) */
    char              icao[8];          /* ICAO code (airports only) */
    struct NavSpatialEntry* next;       /* Next entry in same grid cell chain */
} NavSpatialEntry;

/** Grid-based spatial hash for O(1) nearby-navigation-data queries. */
typedef struct SpatialHash {
    HashTable*        grid_map;         /* grid_key_str → NavSpatialEntry* head */
    int               total_entries;    /* Total entries inserted */
} SpatialHash;

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

/**
 * @brief Create an empty spatial hash.
 * @param bucket_count  Number of hash buckets for the grid map (use a prime
 *                      like 1009 — scales to ~235k entries with ~230/bucket).
 * @return Heap-allocated SpatialHash*, or NULL on failure.
 */
SpatialHash* spatial_hash_create(int bucket_count);

/**
 * @brief Destroy the spatial hash and free all entry memory.
 */
void spatial_hash_destroy(SpatialHash* sh);

/* =========================================================================
 *  Operations
 * ========================================================================= */

/**
 * @brief Insert a copy of a navigation entry into the spatial hash.
 *
 * The entry is deep-copied to the heap. The grid key is computed from
 * lat/lon and the entry is prepended to the corresponding grid cell chain.
 *
 * @param sh     Spatial hash.
 * @param entry  Entry data (copied internally).
 * @return 0 on success, -1 on allocation failure.
 */
int spatial_hash_insert(SpatialHash* sh, const NavSpatialEntry* entry);

/**
 * @brief Compute the 64-bit grid key for a position.
 *
 * lat_band  = (int32_t)floor((lat + 90.0) / 1.0)   // 0..180
 * lon_width = 1.0 / max(cos(lat_rad), 0.05)         // cap near poles
 * lon_sec   = (int32_t)floor((lon + 180.0) / lon_width)
 * key       = (lat_band << 32) | (lon_sec & 0xFFFFFFFF)
 */
uint64_t spatial_grid_key(double lat, double lon);

/**
 * @brief Query navigation entries within range_nm of (lat, lon).
 *
 * Uses heading-prioritized neighbor-grid search:
 *   1. Find current grid cell
 *   2. Collect 3×3 neighbor cells
 *   3. Sort neighbors by cosine similarity to heading (closest alignment first)
 *   4. Walk each cell's entry chain, compute distance, collect matches
 *   5. Stop when max_results entries collected
 *
 * @param sh           Spatial hash.
 * @param lat, lon     Query position (degrees).
 * @param heading_deg  Current heading (°), for search prioritization.
 * @param range_nm     Search radius in nautical miles.
 * @param results      Output array (caller-allocated, size ≥ max_results).
 * @param max_results  Maximum number of results to return.
 * @return Number of results written to results[] (0..max_results).
 */
int spatial_hash_query(const SpatialHash* sh,
                       double lat, double lon,
                       float heading_deg, float range_nm,
                       NavQueryResult* results, int max_results);

/**
 * @brief Query count only (no result data) — fast check for nearby entries.
 */
int spatial_hash_count_nearby(const SpatialHash* sh,
                              double lat, double lon, float range_nm);

#endif /* SPATIAL_HASH_H */
