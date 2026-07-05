/**
 * @file    navdata.c
 * @brief   Navigation data operations implementation.
 *
 * All geographic calculations use the spherical Earth model
 * (radius = 3440.065 NM, which is 6371 km / 1.852).
 */

#include "navdata.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Earth radius in nautical miles (6371 km / 1.852) */
#define EARTH_RADIUS_NM 3440.065f
#define DEG2RAD(d)      ((d) * M_PI / 180.0)
#define RAD2DEG(r)      ((r) * 180.0 / M_PI)

/* =========================================================================
 *  Geographic calculations
 * ========================================================================= */

double geo_distance_nm(GeoPos a, GeoPos b)
{
    double lat1 = DEG2RAD(a.lat_deg);
    double lon1 = DEG2RAD(a.lon_deg);
    double lat2 = DEG2RAD(b.lat_deg);
    double lon2 = DEG2RAD(b.lon_deg);

    double dlat = lat2 - lat1;
    double dlon = lon2 - lon1;

    double sin_dlat = sin(dlat / 2.0);
    double sin_dlon = sin(dlon / 2.0);

    double a_val = sin_dlat * sin_dlat +
                   cos(lat1) * cos(lat2) * sin_dlon * sin_dlon;
    double c_val = 2.0 * atan2(sqrt(a_val), sqrt(1.0 - a_val));

    return EARTH_RADIUS_NM * c_val;
}

double geo_bearing_deg(GeoPos a, GeoPos b)
{
    double lat1 = DEG2RAD(a.lat_deg);
    double lon1 = DEG2RAD(a.lon_deg);
    double lat2 = DEG2RAD(b.lat_deg);
    double lon2 = DEG2RAD(b.lon_deg);

    double dlon = lon2 - lon1;
    double y = sin(dlon) * cos(lat2);
    double x = cos(lat1) * sin(lat2) -
               sin(lat1) * cos(lat2) * cos(dlon);

    double bearing = atan2(y, x);
    double deg = RAD2DEG(bearing);

    /* Normalize to 0..360 */
    if (deg < 0.0) deg += 360.0;
    return deg;
}

GeoPos geo_offset(GeoPos start, double dist_nm, double bearing_deg)
{
    double lat1 = DEG2RAD(start.lat_deg);
    double lon1 = DEG2RAD(start.lon_deg);
    double brng = DEG2RAD(bearing_deg);
    double ang_dist = dist_nm / EARTH_RADIUS_NM;

    double lat2 = asin(sin(lat1) * cos(ang_dist) +
                       cos(lat1) * sin(ang_dist) * cos(brng));

    double lon2 = lon1 + atan2(sin(brng) * sin(ang_dist) * cos(lat1),
                                cos(ang_dist) - sin(lat1) * sin(lat2));

    GeoPos result;
    result.lat_deg = RAD2DEG(lat2);
    result.lon_deg = RAD2DEG(lon2);

    /* Normalize longitude */
    if (result.lon_deg > 180.0)  result.lon_deg -= 360.0;
    if (result.lon_deg < -180.0) result.lon_deg += 360.0;

    return result;
}

/* =========================================================================
 *  Navigation database — hardcoded Chinese airports & waypoints
 * ========================================================================= */

int nav_database_init(FMCState* state)
{
    if (!state) return -1;

    /* --- Airports (10 major Chinese airports) --- */

    const struct { const char* icao; const char* iata; const char* name;
                   double lat; double lon; float elev; } apts[] = {
        { "KHIO", "HIO", "Portland Hillsboro",     45.5404, -122.9498, 208 },
        { "KPDX", "PDX", "Portland Intl",         45.5887, -122.5969,  31 },
        { "ZBAA", "PEK", "Beijing Capital",       40.0799, 116.6031,  115 },
        { "ZSSS", "SHA", "Shanghai Hongqiao",     31.1979, 121.3363,   10 },
        { "ZSPD", "PVG", "Shanghai Pudong",       31.1434, 121.8053,   13 },
        { "ZGGG", "CAN", "Guangzhou Baiyun",      23.3925, 113.2988,   50 },
        { "ZUUU", "CTU", "Chengdu Shuangliu",     30.5785, 103.9469, 1625 },
        { "ZBTJ", "TSN", "Tianjin Binhai",        39.1244, 117.3462,   10 },
        { "ZSHC", "HGH", "Hangzhou Xiaoshan",     30.2295, 120.4344,   23 },
        { "ZPPP", "KMG", "Kunming Changshui",     25.1019, 102.9291, 6895 },
        { "ZSAM", "XMN", "Xiamen Gaoqi",          24.5440, 118.1277,   59 },
        { "ZSNJ", "NKG", "Nanjing Lukou",         31.7420, 118.8620,   49 },
    };
    int apt_n = (int)(sizeof(apts) / sizeof(apts[0]));
    if (apt_n > 128) apt_n = 128;

    for (int i = 0; i < apt_n; i++) {
        Airport* a = &state->nav_airports[i];
        strncpy(a->icao, apts[i].icao, sizeof(a->icao) - 1);
        strncpy(a->iata, apts[i].iata, sizeof(a->iata) - 1);
        strncpy(a->name, apts[i].name, sizeof(a->name) - 1);
        a->pos.lat_deg = apts[i].lat;
        a->pos.lon_deg = apts[i].lon;
        a->elevation_ft = apts[i].elev;
        a->mag_var_deg = 0.0f;
        a->runway_count = 0;
        a->longest_runway_ft = 10000.0f;
    }
    state->nav_apt_count = apt_n;

    /* --- Waypoints on Chinese airways --- */

    const struct { const char* ident; int type;
                   double lat; double lon; float elev; float freq; } wpts[] = {
        /* A593: Beijing coastal route to Shanghai */
        { "PILOS", WPT_WAYPOINT, 38.9800, 121.0000,  0, 0 },
        { "LADIX", WPT_WAYPOINT, 37.6800, 121.3000,  0, 0 },
        { "LJG",   WPT_WAYPOINT, 36.0300, 121.8300,  0, 0 },
        { "LAMEN", WPT_WAYPOINT, 34.7200, 122.8800,  0, 0 },
        { "DUOBA", WPT_WAYPOINT, 32.5200, 122.8500,  0, 0 },
        { "AND",   WPT_VOR,      30.3000, 122.3300,  0, 113.90f },
        /* A326: Beijing inland route to Shanghai */
        { "PANKI", WPT_WAYPOINT, 37.2000, 116.5200,  0, 0 },
        { "SUGOL", WPT_WAYPOINT, 33.4300, 118.6800,  0, 0 },
        { "UDINO", WPT_WAYPOINT, 31.8800, 119.9300,  0, 0 },
        /* A470: Shanghai south to Guangzhou */
        { "LUPVI", WPT_WAYPOINT, 29.5000, 120.7500,  0, 0 },
        { "SAVOK", WPT_WAYPOINT, 27.5000, 119.8000,  0, 0 },
        { "FQG",   WPT_VOR,      25.9200, 119.3800,  0, 117.40f },
        { "BUPAN", WPT_WAYPOINT, 24.1000, 117.8000,  0, 0 },
        { "DOTIO", WPT_WAYPOINT, 22.8500, 115.5000,  0, 0 },
        { "BIGRO", WPT_WAYPOINT, 22.1500, 113.7500,  0, 0 },
        /* W51 / general */
        { "IDUXA", WPT_WAYPOINT, 32.1300, 104.1000,  0, 0 },
        { "BOBAK", WPT_WAYPOINT, 38.5000, 115.9800,  0, 0 },
        { "LBN",   WPT_VOR,      25.5900, 119.1500,  0, 115.80f },
    };
    int wpt_n = (int)(sizeof(wpts) / sizeof(wpts[0]));
    if (wpt_n > 512) wpt_n = 512;

    for (int i = 0; i < wpt_n; i++) {
        Waypoint* w = &state->nav_waypoints[i];
        strncpy(w->ident, wpts[i].ident, sizeof(w->ident) - 1);
        w->type         = (WaypointType)wpts[i].type;
        w->pos.lat_deg  = wpts[i].lat;
        w->pos.lon_deg  = wpts[i].lon;
        w->elevation_ft = wpts[i].elev;
        w->freq_mhz     = wpts[i].freq;
    }
    state->nav_wpt_count = wpt_n;

    LOG_INFO("Nav database: %d airports, %d waypoints loaded",
             state->nav_apt_count, state->nav_wpt_count);
    return 0;
}

/* =========================================================================
 *  FMC State
 * ========================================================================= */

FMCState* fmc_state_create(void)
{
    FMCState* s = calloc(1, sizeof(FMCState));
    if (!s) {
        LOG_ERROR("Out of memory allocating FMCState");
        return NULL;
    }

    s->mutex = SDL_CreateMutex();
    if (!s->mutex) {
        LOG_ERROR("SDL_CreateMutex failed for FMCState: %s", SDL_GetError());
        free(s);
        return NULL;
    }

    LOG_DEBUG("FMCState created");
    return s;
}

void fmc_state_free(FMCState* state)
{
    if (!state) return;
    if (state->mutex) SDL_DestroyMutex(state->mutex);
    free(state);
}

void fmc_state_lock(FMCState* s)
{
    if (s && s->mutex) SDL_LockMutex(s->mutex);
}

void fmc_state_unlock(FMCState* s)
{
    if (s && s->mutex) SDL_UnlockMutex(s->mutex);
}

/* --- Flight plan operations -------------------------------------------- */

int flight_plan_add_waypoint(FlightPlan* plan, Waypoint wpt, int after_index)
{
    if (!plan) return -1;
    if (plan->waypoint_count >= MAX_ROUTE_WAYPOINTS) {
        LOG_WARN("Flight plan full (max %d waypoints)", MAX_ROUTE_WAYPOINTS);
        return -1;
    }

    int insert_at;
    if (after_index < 0 || after_index >= plan->waypoint_count) {
        /* Append at end */
        insert_at = plan->waypoint_count;
    } else {
        insert_at = after_index + 1;
        /* Shift later waypoints right */
        memmove(&plan->waypoints[insert_at + 1],
                &plan->waypoints[insert_at],
                (size_t)(plan->waypoint_count - insert_at) * sizeof(Waypoint));
    }

    plan->waypoints[insert_at] = wpt;
    plan->waypoint_count++;
    flight_plan_recalculate(plan);
    return 0;
}

int flight_plan_remove_waypoint(FlightPlan* plan, int index)
{
    if (!plan || index < 0 || index >= plan->waypoint_count) return -1;

    /* Shift later waypoints left */
    memmove(&plan->waypoints[index],
            &plan->waypoints[index + 1],
            (size_t)(plan->waypoint_count - index - 1) * sizeof(Waypoint));
    plan->waypoint_count--;

    /* Clear the vacated slot */
    memset(&plan->waypoints[plan->waypoint_count], 0, sizeof(Waypoint));
    flight_plan_recalculate(plan);
    return 0;
}

void flight_plan_clear(FlightPlan* plan)
{
    if (!plan) return;
    memset(plan->waypoints, 0, sizeof(plan->waypoints));
    plan->waypoint_count = 0;
    plan->active_waypoint_index = -1;
    plan->total_distance_nm = 0.0f;
    plan->estimated_time_hours = 0.0f;
    plan->fuel_required_lbs = 0.0f;
    /* FMCState::plan_modified must be set by the caller */
}

void flight_plan_recalculate(FlightPlan* plan)
{
    if (!plan || plan->waypoint_count < 2) {
        if (plan) {
            plan->total_distance_nm = 0.0f;
            plan->estimated_time_hours = 0.0f;
        }
        return;
    }

    double total_nm = 0.0;
    for (int i = 0; i < plan->waypoint_count - 1; i++) {
        total_nm += geo_distance_nm(plan->waypoints[i].pos,
                                    plan->waypoints[i + 1].pos);
    }
    plan->total_distance_nm = (float)total_nm;

    if (plan->cruise_speed_kts > 0.0f) {
        plan->estimated_time_hours = (float)(total_nm / plan->cruise_speed_kts);
    }

    /* Rough fuel estimate: 5000 lbs/hour as default burn rate */
    float fuel_burn_pph = 5000.0f;
    plan->fuel_required_lbs = plan->estimated_time_hours * fuel_burn_pph;
}

/* --- Serialization ----------------------------------------------------- */

int flight_plan_save(const FlightPlan* plan, const char* path)
{
    if (!plan || !path) return -1;

    FILE* fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("Cannot open %s for writing flight plan", path);
        return -1;
    }

    fprintf(fp, "# Flight Plan\n");
    fprintf(fp, "flight_number=%s\n", plan->flight_number);
    fprintf(fp, "aircraft_type=%s\n", plan->aircraft_type);
    fprintf(fp, "departure=%s\n", plan->departure.icao);
    fprintf(fp, "arrival=%s\n", plan->arrival.icao);
    fprintf(fp, "cruise_alt=%f\n", (double)plan->cruise_altitude_ft);
    fprintf(fp, "cruise_spd=%f\n", (double)plan->cruise_speed_kts);
    fprintf(fp, "waypoint_count=%d\n", plan->waypoint_count);

    for (int i = 0; i < plan->waypoint_count; i++) {
        const Waypoint* w = &plan->waypoints[i];
        fprintf(fp, "wpt[%d]=%s,%d,%.6f,%.6f,%.1f,%.2f\n",
                i, w->ident, (int)w->type,
                w->pos.lat_deg, w->pos.lon_deg,
                (double)w->elevation_ft, (double)w->freq_mhz);
    }

    fclose(fp);
    LOG_INFO("Flight plan saved to %s (%d waypoints)", path, plan->waypoint_count);
    return 0;
}

int flight_plan_load(FlightPlan* plan, const char* path)
{
    if (!plan || !path) return -1;

    FILE* fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN("Flight plan file not found: %s", path);
        return -1;
    }

    flight_plan_clear(plan);

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';

        if (line[0] == '#' || line[0] == '\0') continue;

        char key[64] = {0};
        char val[192] = {0};

        if (sscanf(line, "%63[^=]=%191[^\n]", key, val) == 2) {
            if (strcmp(key, "flight_number") == 0) {
                strncpy(plan->flight_number, val, sizeof(plan->flight_number) - 1);
            } else if (strcmp(key, "aircraft_type") == 0) {
                strncpy(plan->aircraft_type, val, sizeof(plan->aircraft_type) - 1);
            } else if (strcmp(key, "departure") == 0) {
                strncpy(plan->departure.icao, val, sizeof(plan->departure.icao) - 1);
            } else if (strcmp(key, "arrival") == 0) {
                strncpy(plan->arrival.icao, val, sizeof(plan->arrival.icao) - 1);
            } else if (strcmp(key, "cruise_alt") == 0) {
                plan->cruise_altitude_ft = (float)atof(val);
            } else if (strcmp(key, "cruise_spd") == 0) {
                plan->cruise_speed_kts = (float)atof(val);
            } else if (strcmp(key, "waypoint_count") == 0) {
                /* Read the specified number of waypoints */
                int count = atoi(val);
                for (int i = 0; i < count; i++) {
                    char wpt_line[256];
                    if (!fgets(wpt_line, sizeof(wpt_line), fp)) break;
                    wpt_line[strcspn(wpt_line, "\r\n")] = '\0';

                    Waypoint w;
                    memset(&w, 0, sizeof(w));
                    int idx, type;
                    if (sscanf(wpt_line, "wpt[%d]=%7[^,],%d,%lf,%lf,%f,%f",
                               &idx, w.ident, &type,
                               &w.pos.lat_deg, &w.pos.lon_deg,
                               &w.elevation_ft, &w.freq_mhz) == 7) {
                        w.type = (WaypointType)type;
                        flight_plan_add_waypoint(plan, w, -1);
                    }
                }
            }
        }
    }

    fclose(fp);
    flight_plan_recalculate(plan);
    LOG_INFO("Flight plan loaded from %s (%d waypoints, %.0f NM)",
             path, plan->waypoint_count, (double)plan->total_distance_nm);
    return 0;
}
