/**
 * @file    navdata.h
 * @brief   Navigation data structures and operations.
 *
 * Defines data types for airports, runways, waypoints, navaids,
 * and flight routes. Used by FMC, ND, and map display.
 *
 * All geographic coordinates use degrees (not radians) with the
 * convention: lat [-90..+90], lon [-180..+180].
 */

#ifndef NAVDATA_H
#define NAVDATA_H

#include <stdint.h>
#include <SDL2/SDL.h>

/* =========================================================================
 *  Basic geo types
 * ========================================================================= */

/** Geographic coordinate */
typedef struct {
    double lat_deg;
    double lon_deg;
} GeoPos;

/** Calculate great-circle distance between two positions (nautical miles). */
double geo_distance_nm(GeoPos a, GeoPos b);

/** Calculate initial true bearing from a to b (degrees 0..360). */
double geo_bearing_deg(GeoPos a, GeoPos b);

/** Given start, distance (NM), and bearing (°), compute destination. */
GeoPos geo_offset(GeoPos start, double dist_nm, double bearing_deg);

/* =========================================================================
 *  Waypoint types
 * ========================================================================= */

typedef enum {
    WPT_WAYPOINT = 0,   /* Named fix (e.g. "FIX01") */
    WPT_VOR,            /* VOR station */
    WPT_NDB,            /* NDB station */
    WPT_AIRPORT,        /* Airport reference point */
    WPT_LATLON          /* User-defined lat/lon waypoint */
} WaypointType;

typedef struct {
    char         ident[8];      /* Identifier (e.g. "SHA", "ZBAA") */
    WaypointType type;
    GeoPos       pos;
    float        elevation_ft;  /* Station/airport elevation */
    float        freq_mhz;      /* Frequency (VOR/NDB only, 0 otherwise) */
    float        mag_var_deg;   /* Magnetic variation at this point */
} Waypoint;

/* =========================================================================
 *  Airport
 * ========================================================================= */

typedef struct {
    char     icao[8];          /* ICAO code (e.g. "ZBAA") */
    char     iata[4];          /* IATA code (e.g. "PEK") */
    char     name[64];         /* Full name */
    GeoPos   pos;
    float    elevation_ft;
    float    mag_var_deg;

    /* Runways — simplified: array of runway identifiers */
    char     runways[16][8];   /* e.g. "18L/36R" */
    int      runway_count;

    /* Longest runway length (ft), for reference */
    float    longest_runway_ft;
} Airport;

/* =========================================================================
 *  Route / flight plan
 * ========================================================================= */

typedef enum {
    PROC_NONE = 0,
    PROC_SID,          /* Standard Instrument Departure */
    PROC_STAR,         /* Standard Terminal Arrival Route */
    PROC_APPROACH      /* Approach procedure (ILS, RNAV, etc.) */
} ProcedureType;

typedef struct {
    char           name[32];
    ProcedureType  type;
    char           runway[8];        /* Associated runway */
    Waypoint       waypoints[32];    /* Procedure waypoints */
    int            waypoint_count;
} Procedure;

/** Maximum waypoints in a flight plan */
#define MAX_ROUTE_WAYPOINTS 128
#define MAX_ROUTE_ALTN       4   /* Max alternate airports */

typedef struct {
    /* Departure & arrival */
    Airport       departure;
    Airport       arrival;
    Airport       alternate[MAX_ROUTE_ALTN];
    int           alternate_count;

    /* Enroute waypoints (includes departure & arrival airports at ends) */
    Waypoint      waypoints[MAX_ROUTE_WAYPOINTS];
    int           waypoint_count;

    /* Departure & arrival procedures */
    Procedure     sid;
    Procedure     star;
    Procedure     approach;

    /* Flight plan metadata */
    char          flight_number[16];
    char          aircraft_type[16];
    float         cruise_altitude_ft;
    float         cruise_speed_kts;    /* TAS */

    /* Route statistics (computed) */
    float         total_distance_nm;
    float         estimated_time_hours;
    float         fuel_required_lbs;

    /* Active segment tracking */
    int           active_waypoint_index; /* 0..waypoint_count-1, or -1 */

} FlightPlan;

/* =========================================================================
 *  FMC shared state
 * ========================================================================= */

/**
 * @brief FMC state shared between FMC instrument and data layer.
 *
 * Owned by App, passed to FMC instrument via on_init.
 */
typedef struct FMCState {
    FlightPlan     flight_plan;
    int            plan_modified;    /* 1 when flight plan changed */
    char           scratchpad[32];   /* FMC scratchpad content */

    /* Message system */
    char           message_line[64]; /* Current FMC message */
    uint32_t       message_ticks;    /* When message was set (for timeout) */

    /* Navigation database */
    Waypoint       nav_waypoints[512];
    int            nav_wpt_count;
    Airport        nav_airports[256];
    int            nav_apt_count;

    /* State */
    SDL_mutex*     mutex;
} FMCState;

/* --- FMC State API ----------------------------------------------------- */

FMCState* fmc_state_create(void);
void      fmc_state_free(FMCState* state);

/**
 * @brief Initialize the navigation database with hardcoded airports
 *        and waypoints covering major Chinese airways.
 *        Must be called once after fmc_state_create() and before
 *        any instrument on_init.
 * @param state  FMC state to populate.
 * @return 0 on success, -1 on error.
 */
int       nav_database_init(FMCState* state);

/* Thread-safe flight plan access */
void      fmc_state_lock(FMCState* s);
void      fmc_state_unlock(FMCState* s);

/* Flight plan operations */
int       flight_plan_add_waypoint(FlightPlan* plan, Waypoint wpt, int after_index);
int       flight_plan_remove_waypoint(FlightPlan* plan, int index);
void      flight_plan_clear(FlightPlan* plan);
void      flight_plan_recalculate(FlightPlan* plan);

/* Serialization */
int       flight_plan_save(const FlightPlan* plan, const char* path);
int       flight_plan_load(FlightPlan* plan, const char* path);

#endif /* NAVDATA_H */
