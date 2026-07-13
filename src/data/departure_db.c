/**
 * @file    departure_db.c
 * @brief   Departure procedure database — loads fmc_data.txt.
 */

#include "departure_db.h"
#include "utils/logger.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* =========================================================================
 *  Parser — reads the structured text format from fmc_data.txt
 * ========================================================================= */

static void trim_line(char* line)
{
    /* Strip trailing \r \n */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
        line[--len] = '\0';

    /* Strip '#' comments (everything from # to end) */
    char* hash = strchr(line, '#');
    if (hash) { *hash = '\0'; len = (size_t)(hash - line); }

    /* Strip trailing spaces */
    while (len > 0 && line[len-1] == ' ')
        line[--len] = '\0';
}

int dep_db_load(DepartureDB* db, const char* path)
{
    if (!db || !path) return -1;
    memset(db, 0, sizeof(*db));

    FILE* f = fopen(path, "r");
    if (!f) {
        LOG_WARN("dep_db: cannot open %s", path);
        return -1;
    }

    char line[256];
    DepartureAirport* cur = NULL;

    while (fgets(line, sizeof(line), f)) {
        trim_line(line);

        /* Skip empty lines and comments */
        if (line[0] == '\0' || line[0] == '#') continue;

        char token[32] = {0};
        /* Extract first word as token */
        sscanf(line, "%31s", token);

        if (strcmp(token, "AIRPORT") == 0) {
            if (db->count >= DEP_DB_MAX_AIRPORTS) {
                LOG_WARN("dep_db: max airports (%d) reached", DEP_DB_MAX_AIRPORTS);
                break;
            }
            cur = &db->airports[db->count++];
            memset(cur, 0, sizeof(*cur));
            sscanf(line, "AIRPORT %7s", cur->icao);
        }
        else if (strcmp(token, "RWY") == 0 && cur) {
            if (cur->runway_count >= DEP_DB_MAX_RUNWAYS) continue;
            sscanf(line, "RWY %7s", cur->runways[cur->runway_count]);
            cur->runway_count++;
        }
        else if (strcmp(token, "SID") == 0 && cur) {
            if (cur->sid_count >= DEP_DB_MAX_SIDS) continue;
            /* SID name can have spaces — read rest of line after "SID " */
            const char* name = line + 4;  /* skip "SID " */
            while (*name == ' ') name++;
            strncpy(cur->sids[cur->sid_count], name, 15);
            cur->sids[cur->sid_count][15] = '\0';
            cur->sid_count++;
        }
        else if (strcmp(token, "WPT") == 0 && cur) {
            /* Parse: WPT <ident> <lat> <lon> */
            if (cur->wpt_count >= DEP_DB_MAX_WPTS) continue;
            DepWpt* w = &cur->wpts[cur->wpt_count];
            memset(w, 0, sizeof(*w));
            double lat = 0.0, lon = 0.0;
            if (sscanf(line, "WPT %7s %lf %lf", w->ident, &lat, &lon) >= 3) {
                w->lat_deg = lat;
                w->lon_deg = lon;
            } else {
                /* Backward compat: no coordinates */
                sscanf(line, "WPT %7s", w->ident);
            }
            cur->wpt_count++;
        }
        else if (strcmp(token, "SIDSEQ") == 0 && cur) {
            /* Parse: SIDSEQ <sid_name> <wpt_ident>
             * Appends wpt to the named SID's waypoint sequence */
            char sid[32] = {0}, wpt[32] = {0};
            /* SID name may contain spaces — read rest after "SIDSEQ " */
            const char* rest = line + 7;  /* skip "SIDSEQ " */
            while (*rest == ' ' || *rest == '\t') rest++;
            /* Find the last space to split SID name from wpt ident */
            const char* last_space = strrchr(rest, ' ');
            if (last_space) {
                size_t sid_len = (size_t)(last_space - rest);
                /* Trim trailing spaces from sid name */
                while (sid_len > 0 && (rest[sid_len-1] == ' ' || rest[sid_len-1] == '\t'))
                    sid_len--;
                if (sid_len >= sizeof(sid)) sid_len = sizeof(sid) - 1;
                memcpy(sid, rest, sid_len);
                sid[sid_len] = '\0';
                sscanf(last_space + 1, "%31s", wpt);
            } else {
                /* Fallback: single-word names */
                sscanf(line, "SIDSEQ %31s %31s", sid, wpt);
            }

            if (sid[0] && wpt[0]) {
                /* Find or create SID sequence entry */
                DepSidSeq* seq = NULL;
                for (int i = 0; i < cur->sid_seq_count; i++) {
                    if (strcmp(cur->sid_seq[i].sid, sid) == 0) {
                        seq = &cur->sid_seq[i];
                        break;
                    }
                }
                if (!seq && cur->sid_seq_count < DEP_DB_MAX_SIDS) {
                    seq = &cur->sid_seq[cur->sid_seq_count++];
                    memset(seq, 0, sizeof(*seq));
                    strncpy(seq->sid, sid, sizeof(seq->sid) - 1);
                }
                if (seq && seq->wpt_count < DEP_DB_MAX_SIDSEQ) {
                    strncpy(seq->wpts[seq->wpt_count], wpt,
                            sizeof(seq->wpts[0]) - 1);
                    seq->wpt_count++;
                }
            }
        }
        else if (strcmp(token, "LINK") == 0 && cur) {
            /* Parse "LINK  <A> -> <B>" — split on "->" to handle spaces in names */
            char* arrow = strstr(line, "->");
            if (arrow) {
                char a[32] = {0}, b[32] = {0};

                /* Extract left side: skip "LINK " (5 chars), up to arrow */
                char* a_start = line + 4;  /* after "LINK" */
                while (*a_start == ' ' || *a_start == '\t') a_start++;
                size_t a_len = (size_t)(arrow - a_start);
                while (a_len > 0 && (a_start[a_len-1] == ' ' || a_start[a_len-1] == '\t'))
                    a_len--;
                if (a_len >= sizeof(a)) a_len = sizeof(a) - 1;
                memcpy(a, a_start, a_len);
                a[a_len] = '\0';

                /* Extract right side: after "->" */
                char* b_start = arrow + 2;
                while (*b_start == ' ' || *b_start == '\t') b_start++;
                size_t b_len = strlen(b_start);
                while (b_len > 0 && (b_start[b_len-1] == ' ' || b_start[b_len-1] == '\t'))
                    b_len--;
                if (b_len >= sizeof(b)) b_len = sizeof(b) - 1;
                memcpy(b, b_start, b_len);
                b[b_len] = '\0';

                if (a[0] && b[0]) {
                    /* Determine if this is runway→sid, sid→wpt, or runway→wpt */
                    int a_is_runway = 0, a_is_sid = 0;
                    int b_is_sid = 0, b_is_wpt = 0;

                    for (int i = 0; i < cur->runway_count; i++)
                        if (strcmp(a, cur->runways[i]) == 0) { a_is_runway = 1; break; }
                    for (int i = 0; i < cur->sid_count; i++)
                        if (strcmp(a, cur->sids[i]) == 0) { a_is_sid = 1; break; }
                    for (int i = 0; i < cur->sid_count; i++)
                        if (strcmp(b, cur->sids[i]) == 0) { b_is_sid = 1; break; }

                    b_is_wpt = !b_is_sid;
                    for (int i = 0; i < cur->runway_count; i++)
                        if (strcmp(b, cur->runways[i]) == 0) { b_is_wpt = 0; break; }

                    if (a_is_runway && b_is_sid) {
                        if (cur->rwy_sid_count < DEP_DB_MAX_LINKS) {
                            strncpy(cur->rwy_sid[cur->rwy_sid_count].runway, a, 7);
                            cur->rwy_sid[cur->rwy_sid_count].runway[7] = '\0';
                            strncpy(cur->rwy_sid[cur->rwy_sid_count].sid, b, 15);
                            cur->rwy_sid[cur->rwy_sid_count].sid[15] = '\0';
                            cur->rwy_sid_count++;
                        }
                    } else if (a_is_sid && b_is_wpt) {
                        if (cur->sid_wpt_count < DEP_DB_MAX_LINKS) {
                            strncpy(cur->sid_wpt[cur->sid_wpt_count].sid, a, 15);
                            cur->sid_wpt[cur->sid_wpt_count].sid[15] = '\0';
                            strncpy(cur->sid_wpt[cur->sid_wpt_count].wpt, b, 7);
                            cur->sid_wpt[cur->sid_wpt_count].wpt[7] = '\0';
                            cur->sid_wpt_count++;
                        }
                    } else if (a_is_runway && b_is_wpt) {
                        if (cur->rwy_wpt_count < DEP_DB_MAX_LINKS) {
                            strncpy(cur->rwy_wpt[cur->rwy_wpt_count].runway, a, 7);
                            cur->rwy_wpt[cur->rwy_wpt_count].runway[7] = '\0';
                            strncpy(cur->rwy_wpt[cur->rwy_wpt_count].wpt, b, 7);
                            cur->rwy_wpt[cur->rwy_wpt_count].wpt[7] = '\0';
                            cur->rwy_wpt_count++;
                        }
                    }
                }
            }
        }
    }

    fclose(f);
    LOG_INFO("dep_db: loaded %d airports from %s", db->count, path);
    for (int i = 0; i < db->count; i++) {
        DepartureAirport* a = &db->airports[i];
        LOG_INFO("  %s: %d rwy, %d sid, %d wpt, %d rwy-sid, %d sidseq",
                 a->icao, a->runway_count, a->sid_count,
                 a->wpt_count, a->rwy_sid_count, a->sid_seq_count);
    }
    return 0;
}

/* --- Query API ----------------------------------------------------------- */

const DepartureAirport* dep_db_find(const DepartureDB* db, const char* icao)
{
    if (!db || !icao) return NULL;
    for (int i = 0; i < db->count; i++) {
        if (strcmp(db->airports[i].icao, icao) == 0)
            return &db->airports[i];
    }
    return NULL;
}

int dep_db_get_sids_for_runway(const DepartureAirport* apt, const char* runway,
                               char sids_out[DEP_DB_MAX_SIDS][16])
{
    if (!apt || !runway || !sids_out) return -1;
    int count = 0;
    for (int i = 0; i < apt->rwy_sid_count && count < DEP_DB_MAX_SIDS; i++) {
        if (strcmp(apt->rwy_sid[i].runway, runway) == 0) {
            strncpy(sids_out[count], apt->rwy_sid[i].sid, 15);
            sids_out[count][15] = '\0';
            count++;
        }
    }
    return count;
}

int dep_db_get_trans_for_sid(const DepartureAirport* apt, const char* sid,
                             char wpts_out[DEP_DB_MAX_LINKS][8])
{
    if (!apt || !sid || !wpts_out) return -1;
    int count = 0;
    for (int i = 0; i < apt->sid_wpt_count && count < DEP_DB_MAX_LINKS; i++) {
        if (strcmp(apt->sid_wpt[i].sid, sid) == 0) {
            strncpy(wpts_out[count], apt->sid_wpt[i].wpt, 7);
            wpts_out[count][7] = '\0';
            count++;
        }
    }
    return count;
}

int dep_db_get_trans_for_runway(const DepartureAirport* apt, const char* runway,
                                char wpts_out[DEP_DB_MAX_LINKS][8])
{
    if (!apt || !runway || !wpts_out) return -1;
    int count = 0;
    for (int i = 0; i < apt->rwy_wpt_count && count < DEP_DB_MAX_LINKS; i++) {
        if (strcmp(apt->rwy_wpt[i].runway, runway) == 0) {
            strncpy(wpts_out[count], apt->rwy_wpt[i].wpt, 7);
            wpts_out[count][7] = '\0';
            count++;
        }
    }
    return count;
}

int dep_db_find_wpt(const DepartureAirport* apt, const char* ident,
                    double* lat, double* lon)
{
    if (!apt || !ident || !lat || !lon) return 0;
    for (int i = 0; i < apt->wpt_count; i++) {
        if (strcmp(apt->wpts[i].ident, ident) == 0) {
            *lat = apt->wpts[i].lat_deg;
            *lon = apt->wpts[i].lon_deg;
            return 1;
        }
    }
    return 0;
}

int dep_db_get_sid_sequence(const DepartureAirport* apt, const char* sid,
                            char wpts_out[DEP_DB_MAX_SIDSEQ][8])
{
    if (!apt || !sid || !wpts_out) return -1;
    for (int i = 0; i < apt->sid_seq_count; i++) {
        if (strcmp(apt->sid_seq[i].sid, sid) == 0) {
            int n = apt->sid_seq[i].wpt_count;
            for (int j = 0; j < n && j < DEP_DB_MAX_SIDSEQ; j++) {
                strncpy(wpts_out[j], apt->sid_seq[i].wpts[j], 7);
                wpts_out[j][7] = '\0';
            }
            return n;
        }
    }
    return -1;
}
