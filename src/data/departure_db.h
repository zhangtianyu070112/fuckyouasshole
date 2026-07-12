/**
 * @file    departure_db.h
 * @brief   Hardcoded departure procedure (SID) database for FMC DEP/ARR page.
 *
 * Loads runway→SID→transition mappings from fmc_data.txt.
 * Currently covers 4 airports: KSEA, KBFI, ZUUU, ZUCK.
 */

#ifndef DEPARTURE_DB_H
#define DEPARTURE_DB_H

#include <stdint.h>

#define DEP_DB_MAX_RUNWAYS  16
#define DEP_DB_MAX_SIDS     16
#define DEP_DB_MAX_LINKS    32
#define DEP_DB_MAX_AIRPORTS 8

/* --- Data types ---------------------------------------------------------- */

typedef struct {
    char icao[8];
    char runways[DEP_DB_MAX_RUNWAYS][8];
    int  runway_count;
    char sids[DEP_DB_MAX_SIDS][16];
    int  sid_count;

    /* Runway → SID links */
    struct {
        char runway[8];
        char sid[16];
    } rwy_sid[DEP_DB_MAX_LINKS];
    int  rwy_sid_count;

    /* SID → waypoint (transition) links */
    struct {
        char sid[16];
        char wpt[8];
    } sid_wpt[DEP_DB_MAX_LINKS];
    int  sid_wpt_count;

    /* Runway → waypoint (transition) links */
    struct {
        char runway[8];
        char wpt[8];
    } rwy_wpt[DEP_DB_MAX_LINKS];
    int  rwy_wpt_count;
} DepartureAirport;

typedef struct DepartureDB {
    DepartureAirport airports[DEP_DB_MAX_AIRPORTS];
    int              count;
} DepartureDB;

/* --- API ----------------------------------------------------------------- */

/** Load departure database from a text file. Returns 0 on success. */
int dep_db_load(DepartureDB* db, const char* path);

/** Find an airport by ICAO code. Returns NULL if not found. */
const DepartureAirport* dep_db_find(const DepartureDB* db, const char* icao);

/**
 * @brief Get SIDs available for a given runway.
 * @param apt     Airport to query.
 * @param runway  Runway identifier (e.g. "RW02L").
 * @param sids_out Output array (caller-allocated, DEP_DB_MAX_SIDS × 16).
 * @return Number of SIDs written, or -1 on error.
 */
int dep_db_get_sids_for_runway(const DepartureAirport* apt, const char* runway,
                               char sids_out[DEP_DB_MAX_SIDS][16]);

/**
 * @brief Get transition waypoints for a given SID.
 * @param apt      Airport to query.
 * @param sid      SID name (e.g. "CTU 1X").
 * @param wpts_out Output array (caller-allocated, DEP_DB_MAX_LINKS × 8).
 * @return Number of transitions written, or -1 on error.
 */
int dep_db_get_trans_for_sid(const DepartureAirport* apt, const char* sid,
                             char wpts_out[DEP_DB_MAX_LINKS][8]);

/**
 * @brief Get transition waypoints for a given runway.
 */
int dep_db_get_trans_for_runway(const DepartureAirport* apt, const char* runway,
                                char wpts_out[DEP_DB_MAX_LINKS][8]);

#endif /* DEPARTURE_DB_H */
