/**
 * @file    spatial_hash.c
 * @brief   Grid-based spatial hash table — implementation.
 */

#include "spatial_hash.h"
#include "../data/navdata.h"       /* geo_distance_nm, geo_bearing_deg */
#include "../utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* =========================================================================
 *  Constants
 * ========================================================================= */

#define EARTH_RADIUS_NM   3440.065
#define DEG2RAD(d)        ((d) * M_PI / 180.0)
#define RAD2DEG(r)        ((r) * 180.0 / M_PI)
#define LAT_GRID_DEG      1.0        /* Latitude band height in degrees */
#define MAX_NEIGHBORS     49         /* 7×7 grid coverage (~180 NM radius) */
#define MAX_RESULT_CAP    512        /* Hard cap to protect memory */

/* =========================================================================
 *  Internal helpers
 * ========================================================================= */

/**
 * @brief Quick cosine with protection near poles.
 */
static double safe_cos_lat(double lat_rad)
{
    double c = cos(lat_rad);
    if (c < 0.05) c = 0.05;   /* Cap near ±90° — 1 grid cell ≈ 20° lon at 87°N */
    return c;
}

/* =========================================================================
 *  Grid key
 * ========================================================================= */

uint64_t spatial_grid_key(double lat, double lon)
{
    double lat_rad = DEG2RAD(lat);
    double cos_lat = safe_cos_lat(lat_rad);
    double lon_width = 1.0 / cos_lat;  /* degrees of lon for ~60 NM cell width */

    int32_t lat_band = (int32_t)floor((lat + 90.0) / LAT_GRID_DEG);
    if (lat_band < 0)   lat_band = 0;
    if (lat_band > 180) lat_band = 180;

    int32_t lon_sec = (int32_t)floor((lon + 180.0) / lon_width);
    if (lon_sec < 0) lon_sec = 0;

    return ((uint64_t)(uint32_t)lat_band << 32) | (uint64_t)(uint32_t)lon_sec;
}

/**
 * @brief Convert a grid key back to approximate center coordinates.
 */
static void grid_key_to_center(uint64_t key, double* lat, double* lon)
{
    int32_t lat_band = (int32_t)(key >> 32);
    int32_t lon_sec  = (int32_t)(key & 0xFFFFFFFF);

    double center_lat = (double)lat_band * LAT_GRID_DEG - 90.0 + LAT_GRID_DEG / 2.0;
    double lat_rad = DEG2RAD(center_lat);
    double cos_lat = safe_cos_lat(lat_rad);
    double lon_width = 1.0 / cos_lat;
    double center_lon = (double)lon_sec * lon_width - 180.0 + lon_width / 2.0;

    *lat = center_lat;
    *lon = center_lon;
}

/* =========================================================================
 *  Grid key → string (for hash table key)
 * ========================================================================= */

static void grid_key_to_str(uint64_t key, char* buf, size_t buf_sz)
{
    /* Cast to unsigned long long for portable printf across compilers */
    snprintf(buf, buf_sz, "%llu", (unsigned long long)key);
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 *  Neighbor collection for heading-prioritized search
 * ========================================================================= */

typedef struct {
    uint64_t key;
    double   bearing;    /* Bearing from current position to grid center */
    double   cos_sim;    /* Cosine similarity with heading */
} GridNeighbor;

static int compare_neighbors(const void* a, const void* b)
{
    const GridNeighbor* na = (const GridNeighbor*)a;
    const GridNeighbor* nb = (const GridNeighbor*)b;
    /* Descending: higher cosine similarity first */
    if (na->cos_sim > nb->cos_sim) return -1;
    if (na->cos_sim < nb->cos_sim) return 1;
    return 0;
}

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

SpatialHash* spatial_hash_create(int bucket_count)
{
    if (bucket_count <= 0) bucket_count = 1009;

    SpatialHash* sh = calloc(1, sizeof(SpatialHash));
    if (!sh) {
        LOG_ERROR("spatial_hash_create: out of memory");
        return NULL;
    }

    sh->grid_map = ht_create(bucket_count);
    if (!sh->grid_map) {
        LOG_ERROR("spatial_hash_create: ht_create failed");
        free(sh);
        return NULL;
    }

    sh->total_entries = 0;
    LOG_DEBUG("SpatialHash created (%d buckets)", bucket_count);
    return sh;
}

void spatial_hash_destroy(SpatialHash* sh)
{
    if (!sh) return;

    /* Walk all hash table buckets, free each chain */
    for (int i = 0; i < sh->grid_map->bucket_count; i++) {
        HTEntry* he = sh->grid_map->buckets[i];
        while (he) {
            HTEntry* next_he = he->next;
            /* Walk the entry chain in this grid cell */
            NavSpatialEntry* entry = (NavSpatialEntry*)he->value;
            while (entry) {
                NavSpatialEntry* next_entry = entry->next;
                free(entry);
                entry = next_entry;
            }
            free(he->key);
            free(he);
            he = next_he;
        }
    }

    /* Clear bucket array to avoid double-free in ht_destroy */
    free(sh->grid_map->buckets);
    sh->grid_map->buckets = NULL;
    sh->grid_map->bucket_count = 0;
    sh->grid_map->size = 0;
    free(sh->grid_map);

    free(sh);
}

/* =========================================================================
 *  Insert
 * ========================================================================= */

int spatial_hash_insert(SpatialHash* sh, const NavSpatialEntry* entry)
{
    if (!sh || !entry) return -1;

    /* Deep-copy the entry */
    NavSpatialEntry* copy = calloc(1, sizeof(NavSpatialEntry));
    if (!copy) return -1;
    memcpy(copy, entry, sizeof(NavSpatialEntry));
    copy->next = NULL;

    /* Compute grid key */
    uint64_t key = spatial_grid_key(entry->lat_deg, entry->lon_deg);
    char key_str[32];
    grid_key_to_str(key, key_str, sizeof(key_str));

    /* Look up existing chain head for this grid cell */
    NavSpatialEntry* head = (NavSpatialEntry*)ht_get(sh->grid_map, key_str);

    if (head) {
        /* Prepend to existing chain */
        copy->next = head;
        /* Update hash table value */
        ht_put(sh->grid_map, key_str, copy);
    } else {
        /* New grid cell — create entry */
        ht_put(sh->grid_map, key_str, copy);
    }

    sh->total_entries++;
    return 0;
}

/* =========================================================================
 *  Query
 * ========================================================================= */

int spatial_hash_query(const SpatialHash* sh,
                       double lat, double lon,
                       float heading_deg, float range_nm,
                       NavSpatialEntry** results, int max_results)
{
    if (!sh || !results || max_results <= 0) return 0;
    if (max_results > MAX_RESULT_CAP) max_results = MAX_RESULT_CAP;

    int found = 0;

    /* --- Step 1: compute current grid key --- */
    uint64_t center_key = spatial_grid_key(lat, lon);

    /* --- Step 2: collect neighbor grid keys (5×5 = 25 cells) --- */
    GridNeighbor neighbors[MAX_NEIGHBORS];
    int neighbor_count = 0;
    int32_t center_lat_band = (int32_t)(center_key >> 32);
    int32_t center_lon_sec  = (int32_t)(center_key & 0xFFFFFFFF);

    for (int dlat = -3; dlat <= 3 && neighbor_count < MAX_NEIGHBORS; dlat++) {
        int32_t lb = center_lat_band + dlat;
        if (lb < 0 || lb > 180) continue;

        for (int dlon = -3; dlon <= 3 && neighbor_count < MAX_NEIGHBORS; dlon++) {
            int32_t ls = center_lon_sec + dlon;
            if (ls < 0) continue;

            uint64_t nkey = ((uint64_t)(uint32_t)lb << 32) | (uint64_t)(uint32_t)ls;

            /* Compute approximate grid center for bearing */
            double gc_lat, gc_lon;
            grid_key_to_center(nkey, &gc_lat, &gc_lon);

            GeoPos ac = { lat, lon };
            GeoPos gc = { gc_lat, gc_lon };
            double bearing = geo_bearing_deg(ac, gc);
            double bearing_rad = DEG2RAD(bearing);
            double hdg_rad = DEG2RAD((double)heading_deg);

            double cos_sim = cos(bearing_rad - hdg_rad);

            neighbors[neighbor_count].key     = nkey;
            neighbors[neighbor_count].bearing  = bearing;
            neighbors[neighbor_count].cos_sim  = cos_sim;
            neighbor_count++;
        }
    }

    /* --- Step 3: sort neighbors by cosine similarity (descending) --- */
    qsort(neighbors, (size_t)neighbor_count, sizeof(GridNeighbor), compare_neighbors);

    /* --- Step 4: walk sorted neighbor cells, collect results --- */
    GeoPos query_pos = { lat, lon };

    for (int ni = 0; ni < neighbor_count && found < max_results; ni++) {
        char key_str[32];
        grid_key_to_str(neighbors[ni].key, key_str, sizeof(key_str));

        NavSpatialEntry* entry = (NavSpatialEntry*)ht_get(sh->grid_map, key_str);
        while (entry && found < max_results) {
            GeoPos ep = { entry->lat_deg, entry->lon_deg };
            double dist_nm = geo_distance_nm(query_pos, ep);

            if (dist_nm <= (double)range_nm) {
                results[found] = entry;
                found++;
            }

            entry = entry->next;
        }
    }

    return found;
}

int spatial_hash_count_nearby(const SpatialHash* sh,
                              double lat, double lon, float range_nm)
{
    if (!sh) return 0;

    uint64_t center_key = spatial_grid_key(lat, lon);
    int32_t center_lat_band = (int32_t)(center_key >> 32);
    int32_t center_lon_sec  = (int32_t)(center_key & 0xFFFFFFFF);

    int count = 0;
    GeoPos query_pos = { lat, lon };

    for (int dlat = -1; dlat <= 1; dlat++) {
        int32_t lb = center_lat_band + dlat;
        if (lb < 0 || lb > 180) continue;

        for (int dlon = -1; dlon <= 1; dlon++) {
            int32_t ls = center_lon_sec + dlon;
            if (ls < 0) continue;

            uint64_t nkey = ((uint64_t)(uint32_t)lb << 32) | (uint64_t)(uint32_t)ls;
            char key_str[32];
            grid_key_to_str(nkey, key_str, sizeof(key_str));

            NavSpatialEntry* entry = (NavSpatialEntry*)ht_get(sh->grid_map, key_str);
            while (entry) {
                GeoPos ep = { entry->lat_deg, entry->lon_deg };
                double dist_nm = geo_distance_nm(query_pos, ep);
                if (dist_nm <= (double)range_nm) {
                    count++;
                }
                entry = entry->next;
            }
        }
    }

    return count;
}
