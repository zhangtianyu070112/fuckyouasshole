/**
 * @file    departure_db.h
 * @brief   Departure procedure (SID) database for FMC DEP/ARR page.
 *
 * Loads runway→SID→waypoint mappings from fmc_data.txt.
 * Supports SID waypoint sequences with coordinates for ND route overlay.
 * Currently covers 4 airports: KSEA, KBFI, ZUUU, ZUCK.
 */

#ifndef DEPARTURE_DB_H
#define DEPARTURE_DB_H

#include <stdint.h>

#define DEP_DB_MAX_RUNWAYS  16
#define DEP_DB_MAX_SIDS     16
#define DEP_DB_MAX_WPTS     32
#define DEP_DB_MAX_LINKS    64
#define DEP_DB_MAX_AIRPORTS 8
#define DEP_DB_MAX_SIDSEQ   8   /* Max waypoints per SID sequence */

/* --- Data types ---------------------------------------------------------- */

/** A waypoint with coordinates */
typedef struct {
    char   ident[8];
    double lat_deg;
    double lon_deg;
} DepWpt;

/** SID waypoint sequence entry */
typedef struct {
    char   sid[16];
    char   wpts[DEP_DB_MAX_SIDSEQ][8];  /* ordered waypoint idents */
    int    wpt_count;
} DepSidSeq;

typedef struct {
    char icao[8];
    char runways[DEP_DB_MAX_RUNWAYS][8];
    int  runway_count;
    char sids[DEP_DB_MAX_SIDS][16];
    int  sid_count;

    /* Waypoints with coordinates */
    DepWpt wpts[DEP_DB_MAX_WPTS];
    int    wpt_count;

    /* SID waypoint sequences */
    DepSidSeq sid_seq[DEP_DB_MAX_SIDS];
    int       sid_seq_count;

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
 * @return Number of SIDs written, or -1 on error.
 */
int dep_db_get_sids_for_runway(const DepartureAirport* apt, const char* runway,
                               char sids_out[DEP_DB_MAX_SIDS][16]);

/**
 * @brief Get transition waypoints for a given SID.
 * @return Number of transitions written, or -1 on error.
 */
int dep_db_get_trans_for_sid(const DepartureAirport* apt, const char* sid,
                             char wpts_out[DEP_DB_MAX_LINKS][8]);

/**
 * @brief Get transition waypoints for a given runway.
 */
int dep_db_get_trans_for_runway(const DepartureAirport* apt, const char* runway,
                                char wpts_out[DEP_DB_MAX_LINKS][8]);

/**
 * @brief Look up a waypoint's coordinates by ident.
 * @param apt    Airport to search.
 * @param ident  Waypoint identifier.
 * @param lat    Output latitude (degrees).
 * @param lon    Output longitude (degrees).
 * @return 1 if found, 0 if not.
 */
int dep_db_find_wpt(const DepartureAirport* apt, const char* ident,
                    double* lat, double* lon);

/**
 * @brief Get the ordered waypoint sequence for a SID.
 * @param apt      Airport to query.
 * @param sid      SID name.
 * @param wpts_out Output array of waypoint idents (caller allocates DEP_DB_MAX_SIDSEQ × 8).
 * @return Number of waypoints in sequence, or -1 on error.
 */
int dep_db_get_sid_sequence(const DepartureAirport* apt, const char* sid,
                            char wpts_out[DEP_DB_MAX_SIDSEQ][8]);

#endif /* DEPARTURE_DB_H */
