#include "standalone_fmc.h"
#include "app.h"
#include "net/xplane.h"
#include "data/flight_data.h"
#include "data/navdata.h"
#include "data/route_graph.h"
#include "ds/avl_tree.h"
#include "ds/linked_list.h"
#include "utils/math_util.h"
#include "utils/font_manager.h"
#include "utils/logger.h"

#include <SDL2/SDL_image.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Colors */
#define COL_WHITE      0xFF, 0xFF, 0xFF, 255
#define COL_CYAN       0x00, 0xFF, 0xFF, 255
#define COL_AMBER      0xFF, 0xC0, 0x00, 255
#define COL_GRAY       0x60, 0x60, 0x68, 255

#define MAX_SCRATCHPAD  24
#define MSG_TIMEOUT_MS  4000
#define MSG_MAX_HISTORY 50

typedef struct {
    char   text[64];
    Uint32 timestamp;
} FmcMessage;

typedef struct {
    int    current_page;
    char   scratchpad[MAX_SCRATCHPAD + 1];
    FMCState* fmc;
    App*    app;
    RouteGraph* graph;
    char   origin_input[8];
    char   dest_input[8];
    int    legs_scroll;
    int    legs_sort_mode;
    AVLTree* wpt_tree;
    LinkedList* message_history;
    int         message_scroll;
    float  smooth_gs;
    float  smooth_alt;
    char   display[12][25];
} FMCData;

typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* bg_texture;
    int win_w;
    int win_h;

    struct {
        float x1, y1, x2, y2;
    } screen_bbox;

    struct {
        char label[32];
        float x1, y1, x2, y2;
    } buttons[100];
    int button_count;

    FMCData* fmc_data;
} StandaloneFMC;

static StandaloneFMC* g_sfmc = NULL;

static void set_col(SDL_Renderer* r, uint8_t R, uint8_t G, uint8_t B, uint8_t A) {
    SDL_SetRenderDrawColor(r, R, G, B, A);
}

static void set_message(FMCData* d, const char* msg) {
    if (!d->message_history) return;
    FmcMessage* m = calloc(1, sizeof(FmcMessage));
    if (!m) return;
    strncpy(m->text, msg, sizeof(m->text) - 1);
    m->text[sizeof(m->text) - 1] = '\0';
    m->timestamp = SDL_GetTicks();
    ll_push_back(d->message_history, m);
    while (ll_size(d->message_history) > MSG_MAX_HISTORY) {
        FmcMessage* old = (FmcMessage*)ll_pop_front(d->message_history);
        free(old);
    }
    d->message_scroll = 0;
}

// =========================================================================
//  Page builders (same as original fmc.c)
// =========================================================================

static void build_ident_page(FMCData* d) {
    memset(d->display, ' ', sizeof(d->display));
    snprintf(d->display[0], 25, " FMC-CDU  VER 2.0      ");
    snprintf(d->display[2], 25, " MODEL   B738          ");
    snprintf(d->display[3], 25, " ENG     CFM56-7B26    ");
    snprintf(d->display[5], 25, " NAV DATA              ");
    snprintf(d->display[6], 25, " %04d WAYPOINTS         ", d->fmc ? d->fmc->nav_wpt_count : 0);
    snprintf(d->display[7], 25, " %04d AIRPORTS          ", d->fmc ? d->fmc->nav_apt_count : 0);
    snprintf(d->display[8], 25, " %04d GRAPH NODES       ", d->graph ? d->graph->node_count : 0);
    snprintf(d->display[10], 25, " <IDENT               ");
    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static void build_rte_page(FMCData* d) {
    memset(d->display, ' ', sizeof(d->display));
    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;
    snprintf(d->display[0], 25, " RTE            1/5    ");
    snprintf(d->display[1], 25, " ORIGIN  %-8s     <L1  ", (fp && fp->departure.icao[0]) ? fp->departure.icao : "----");
    snprintf(d->display[2], 25, " DEST    %-8s      R1> ", (fp && fp->arrival.icao[0]) ? fp->arrival.icao : "----");
    snprintf(d->display[3], 25, " FLT NO  %-8s     <L2  ", (fp && fp->flight_number[0]) ? fp->flight_number : "----");
    snprintf(d->display[4], 25, " CRZ ALT %05.0f FT  R2> ", fp ? (double)fp->cruise_altitude_ft : 35000.0);
    snprintf(d->display[6], 25, " WPT: %03d  DIST: %6.0f  ", fp ? fp->waypoint_count : 0, fp ? (double)fp->total_distance_nm : 0.0);
    snprintf(d->display[7], 25, " ETE: %4.1f HR          ", fp ? (double)fp->estimated_time_hours : 0.0);
    snprintf(d->display[10], 25, " <RTE                  ");
    snprintf(d->display[11], 25, "                ACTIV> ");
    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static int wpt_compare_ident(const void* a, const void* b, void* userdata) {
    (void)userdata;
    return strcmp(((const Waypoint*)a)->ident, ((const Waypoint*)b)->ident);
}

static void rebuild_wpt_tree(FMCData* d) {
    if (!d->wpt_tree) return;
    AVLTree* old = d->wpt_tree;
    d->wpt_tree = avl_create(wpt_compare_ident, NULL);
    avl_destroy(old, 0);
    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;
    if (!fp || fp->waypoint_count == 0) return;
    for (int i = 0; i < fp->waypoint_count; i++) avl_insert(d->wpt_tree, &fp->waypoints[i]);
}

typedef struct { const Waypoint* wpts[128]; int count; } WptCollector;
static int collect_wpt(void* data, void* userdata) {
    WptCollector* c = (WptCollector*)userdata;
    if (c->count < 128) c->wpts[c->count++] = (const Waypoint*)data;
    return 0;
}

static void build_legs_page(FMCData* d) {
    memset(d->display, ' ', sizeof(d->display));
    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;
    int count = fp ? fp->waypoint_count : 0;
    const char* mode_str = d->legs_sort_mode ? "ALPHA" : "RTE";
    snprintf(d->display[0], 25, " LEGS %s    %3d WP    ", mode_str, count);
    if (count == 0) {
        snprintf(d->display[3], 25, "  -- NO ROUTE --       ");
        snprintf(d->display[5], 25, "  USE RTE PAGE TO      ");
        snprintf(d->display[6], 25, "  ENTER ROUTE          ");
    } else if (d->legs_sort_mode) {
        rebuild_wpt_tree(d);
        WptCollector col = {{NULL}, 0};
        avl_inorder(d->wpt_tree, collect_wpt, &col);
        int max_show = 10;
        int start = d->legs_scroll;
        if (start > col.count - max_show) start = col.count - max_show;
        if (start < 0) start = 0;
        for (int i = 0; i < max_show && (start + i) < col.count; i++) {
            const Waypoint* w = col.wpts[start + i];
            int is_active = (fp->active_waypoint_index >= 0 && &fp->waypoints[fp->active_waypoint_index] == w);
            snprintf(d->display[i + 1], 25, "%c%-8s              ", is_active ? '>' : ' ', w->ident[0] ? w->ident : "......");
        }
    } else {
        int max_show = 10;
        int start = d->legs_scroll;
        if (start > count - max_show) start = count - max_show;
        if (start < 0) start = 0;
        for (int i = 0; i < max_show && (start + i) < count; i++) {
            int idx = start + i;
            const Waypoint* w = &fp->waypoints[idx];
            int is_active = (fp->active_waypoint_index == idx);
            float cum_dist = 0.0f;
            for (int j = 1; j <= idx; j++) {
                cum_dist += (float)geo_distance_nm(fp->waypoints[j - 1].pos, fp->waypoints[j].pos);
            }
            snprintf(d->display[i + 1], 25, "%c%-7s %6.0f NM      ", is_active ? '>' : ' ', w->ident[0] ? w->ident : "......", (double)cum_dist);
        }
    }
    if (count > 10) snprintf(d->display[11], 25, " <PG UP  PG DN  SORT> ");
    else snprintf(d->display[11], 25, " <LEGS          SORT> ");
    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static void build_perf_page(FMCData* d) {
    memset(d->display, ' ', sizeof(d->display));
    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;
    snprintf(d->display[0], 25, " PERF INIT       3/5    ");
    snprintf(d->display[1], 25, " CRZ ALT %05.0f FT      ", fp ? (double)fp->cruise_altitude_ft : 35000.0);
    snprintf(d->display[2], 25, " CRZ SPD %03.0f KT      ", fp ? (double)fp->cruise_speed_kts : 450.0);
    snprintf(d->display[3], 25, " FUEL REQ %05.0f LBS    ", fp ? (double)fp->fuel_required_lbs : 0.0);
    snprintf(d->display[4], 25, " TOT DIST %.0f NM       ", fp ? (double)fp->total_distance_nm : 0.0);
    snprintf(d->display[5], 25, " CI      80             ");
    snprintf(d->display[6], 25, " GW      140000 LBS     ");
    snprintf(d->display[7], 25, " RESV    5000 LBS       ");
    if (d->graph) snprintf(d->display[9], 25, " GRAPH: %d NODES       ", d->graph->node_count);
    snprintf(d->display[11], 25, " <PERF                 ");
    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static void build_prog_page(FMCData* d) {
    memset(d->display, ' ', sizeof(d->display));
    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;
    snprintf(d->display[0], 25, " PROGRESS        4/5    ");
    float dtg = fp ? fp->total_distance_nm : 0.0f;
    int active = fp ? fp->active_waypoint_index : -1;
    if (active >= 0 && active < (fp ? fp->waypoint_count : 0) && fp->waypoint_count > 0) {
        dtg = 0.0f;
        for (int j = active; j < fp->waypoint_count - 1; j++) {
            dtg += (float)geo_distance_nm(fp->waypoints[j].pos, fp->waypoints[j + 1].pos);
        }
    }
    snprintf(d->display[2], 25, " DTG    %.0f NM         ", (double)dtg);
    snprintf(d->display[3], 25, " ETA    %4.1f HR        ", fp ? (double)fp->estimated_time_hours : 0.0);
    snprintf(d->display[4], 25, " FUEL   %.0f LBS        ", fp ? (double)fp->fuel_required_lbs : 0.0);
    snprintf(d->display[6], 25, " GS     %.0f KT         ", (double)d->smooth_gs);
    snprintf(d->display[7], 25, " ALT    %.0f FT         ", (double)d->smooth_alt);
    snprintf(d->display[9], 25, " WPT    %d/%d           ", fp ? fp->active_waypoint_index + 1 : 0, fp ? fp->waypoint_count : 0);
    snprintf(d->display[11], 25, " <PROG                 ");
    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static void build_radio_page(FMCData* d) {
    memset(d->display, ' ', sizeof(d->display));
    snprintf(d->display[0], 25, " RADIO            6/6    ");
    snprintf(d->display[2], 25, " COM1  %06.3f          ", 122.800);
    snprintf(d->display[3], 25, " COM2  %06.3f          ", 121.500);
    snprintf(d->display[5], 25, " NAV1  %06.2f          ", 110.90);
    snprintf(d->display[6], 25, " NAV2  %06.2f          ", 113.70);
    snprintf(d->display[8], 25, " XPDR  1200 ALT        ");
    snprintf(d->display[9], 25, " DME   ---- NM         ");
    snprintf(d->display[11], 25, " <RADIO                ");
    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static void build_page(FMCData* d) {
    switch (d->current_page) {
        case 0: build_ident_page(d); break;
        case 1: build_rte_page(d);   break;
        case 2: build_legs_page(d);  break;
        case 3: build_perf_page(d);  break;
        case 4: build_prog_page(d);  break;
        case 5: build_radio_page(d); break;
        default: break;
    }
}

static void rte_activate(FMCData* d) {
    if (!d->fmc || !d->graph) {
        set_message(d, "ERR: NO NAV DATA");
        return;
    }
    FlightPlan* fp = &d->fmc->flight_plan;
    const char* orig = (fp->departure.icao[0]) ? fp->departure.icao : NULL;
    const char* dest = (fp->arrival.icao[0])   ? fp->arrival.icao   : NULL;
    if (!orig || !dest) { set_message(d, "ENTER ORIGIN AND DEST"); return; }

    if (fp->departure.pos.lat_deg == 0.0 && fp->departure.pos.lon_deg == 0.0) {
        int found = 0;
        for (int i = 0; i < d->fmc->nav_apt_count; i++) {
            if (strcmp(d->fmc->nav_airports[i].icao, orig) == 0) {
                fp->departure = d->fmc->nav_airports[i]; found = 1; break;
            }
        }
        if (!found) { set_message(d, "ORIGIN NOT IN DATABASE"); return; }
    }
    if (fp->arrival.pos.lat_deg == 0.0 && fp->arrival.pos.lon_deg == 0.0) {
        int found = 0;
        for (int i = 0; i < d->fmc->nav_apt_count; i++) {
            if (strcmp(d->fmc->nav_airports[i].icao, dest) == 0) {
                fp->arrival = d->fmc->nav_airports[i]; found = 1; break;
            }
        }
        if (!found) { set_message(d, "DEST NOT IN DATABASE"); return; }
    }

    if (fp->cruise_altitude_ft < 100.0f) fp->cruise_altitude_ft = 35000.0f;
    if (fp->cruise_speed_kts < 50.0f)    fp->cruise_speed_kts    = 450.0f;

    set_message(d, "COMPUTING ROUTE...");
    RoutePath path;
    int found = route_graph_find_route(d->graph, orig, dest, fp->cruise_altitude_ft, &path);
    if (!found || path.waypoint_count < 2) { set_message(d, "NO ROUTE FOUND"); return; }

    flight_plan_clear(fp);
    strncpy(fp->departure.icao, orig, sizeof(fp->departure.icao) - 1);
    strncpy(fp->arrival.icao, dest, sizeof(fp->arrival.icao) - 1);
    for (int i = 0; i < path.waypoint_count && i < MAX_ROUTE_WAYPOINTS; i++) {
        fp->waypoints[i] = path.waypoints[i];
        fp->waypoint_count = i + 1;
    }
    fp->total_distance_nm   = path.total_distance_nm;
    fp->estimated_time_hours = path.estimated_time_hours;
    fp->fuel_required_lbs    = path.estimated_time_hours * 5000.0f;
    fp->active_waypoint_index = 0;
    d->fmc->plan_modified     = 1;
    d->legs_scroll = 0;

    char buf[64];
    snprintf(buf, sizeof(buf), "ROUTE OK %d WP %.0f NM", path.waypoint_count, (double)path.total_distance_nm);
    set_message(d, buf);
    if (d->app) xplane_fmc_sync(d->app);
}

static void exec_scratchpad(FMCData* d) {
    if (d->scratchpad[0] == '\0') return;
    char cmd[MAX_SCRATCHPAD + 1];
    strncpy(cmd, d->scratchpad, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    for (char* p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);

    switch (d->current_page) {
    case 1:
        {
            FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;
            if (!fp) break;
            if (fp->departure.icao[0] == '\0') {
                strncpy(fp->departure.icao, cmd, sizeof(fp->departure.icao) - 1);
                set_message(d, "ORIGIN SET");
            } else if (fp->arrival.icao[0] == '\0') {
                strncpy(fp->arrival.icao, cmd, sizeof(fp->arrival.icao) - 1);
                set_message(d, "DEST SET");
            } else {
                rte_activate(d);
            }
        }
        break;
    case 3:
        {
            int alt = atoi(cmd);
            if (alt > 0 && d->fmc) {
                d->fmc->flight_plan.cruise_altitude_ft = (float)alt;
                set_message(d, "CRZ ALT SET");
            }
        }
        break;
    default:
        set_message(d, "ENTER COMMAND");
        break;
    }
    memset(d->scratchpad, 0, sizeof(d->scratchpad));
}

static void handle_lsk(FMCData* d, int side, int line) {
    if (d->app && d->app->xp_send_sock && d->app->xp_host[0]) {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "sim/FMS/key_%c%d", (side == 0) ? 'L' : 'R', line + 1);
        xplane_send_command(d->app->xp_send_sock, d->app->xp_host, d->app->xp_send_port, cmd);
    }
    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;

    switch (d->current_page) {
    case 0: if (side == 0 && line == 5) { d->current_page = 0; } break;
    case 1:
        if (side == 0) {
            switch (line) {
            case 0:
                if (d->scratchpad[0] && fp) {
                    for (char* p = d->scratchpad; *p; p++) *p = (char)toupper((unsigned char)*p);
                    strncpy(fp->departure.icao, d->scratchpad, sizeof(fp->departure.icao) - 1);
                    set_message(d, "ORIGIN SET");
                    memset(d->scratchpad, 0, sizeof(d->scratchpad));
                } else if (fp) strncpy(d->scratchpad, fp->departure.icao, sizeof(d->scratchpad) - 1);
                break;
            case 1:
                if (d->scratchpad[0] && fp) {
                    for (char* p = d->scratchpad; *p; p++) *p = (char)toupper((unsigned char)*p);
                    strncpy(fp->flight_number, d->scratchpad, sizeof(fp->flight_number) - 1);
                    set_message(d, "FLT NO SET");
                    memset(d->scratchpad, 0, sizeof(d->scratchpad));
                } else if (fp && fp->flight_number[0]) strncpy(d->scratchpad, fp->flight_number, sizeof(d->scratchpad) - 1);
                break;
            case 5: d->current_page = 0; break;
            }
        } else {
            switch (line) {
            case 0:
                if (d->scratchpad[0] && fp) {
                    for (char* p = d->scratchpad; *p; p++) *p = (char)toupper((unsigned char)*p);
                    strncpy(fp->arrival.icao, d->scratchpad, sizeof(fp->arrival.icao) - 1);
                    set_message(d, "DEST SET");
                    memset(d->scratchpad, 0, sizeof(d->scratchpad));
                } else if (fp) strncpy(d->scratchpad, fp->arrival.icao, sizeof(d->scratchpad) - 1);
                break;
            case 1:
                if (d->scratchpad[0] && fp) {
                    int alt = atoi(d->scratchpad);
                    if (alt > 0) { fp->cruise_altitude_ft = (float)alt; set_message(d, "CRZ ALT SET"); }
                    memset(d->scratchpad, 0, sizeof(d->scratchpad));
                } else if (fp) snprintf(d->scratchpad, sizeof(d->scratchpad), "%.0f", (double)fp->cruise_altitude_ft);
                break;
            case 5: rte_activate(d); break;
            }
        }
        break;
    case 2:
        if (side == 0 && line == 5) d->current_page = 1;
        if (side == 1 && line == 5) {
            if (fp && fp->waypoint_count > 10) {
                d->legs_scroll += 10;
                if (d->legs_scroll >= fp->waypoint_count) d->legs_scroll = 0;
            }
        }
        break;
    case 3: if (side == 0 && line == 5) d->current_page = 2; break;
    case 4: if (side == 0 && line == 5) d->current_page = 3; break;
    }
}

static void xp_forward_cdu_key(FMCData* d, const char* label) {
    if (!d || !d->app || !d->app->xp_send_sock) return;
    if (d->app->xp_host[0] == '\0') return;
    const char* cmd = NULL;
    if (label[0] >= 'A' && label[0] <= 'Z' && label[1] == '\0') {
        char buf[32]; snprintf(buf, sizeof(buf), "sim/FMS/key_%c", label[0]);
        xplane_send_command(d->app->xp_send_sock, d->app->xp_host, d->app->xp_send_port, buf);
        return;
    }
    if (label[0] >= '0' && label[0] <= '9' && label[1] == '\0') {
        char buf[32]; snprintf(buf, sizeof(buf), "sim/FMS/key_%c", label[0]);
        xplane_send_command(d->app->xp_send_sock, d->app->xp_host, d->app->xp_send_port, buf);
        return;
    }
    if      (strcmp(label, "CLR")       == 0) cmd = "sim/FMS/key_clear";
    else if (strcmp(label, "DEL")       == 0) cmd = "sim/FMS/key_delete";
    else if (strcmp(label, "SP")        == 0) cmd = "sim/FMS/key_space";
    else if (strcmp(label, "/")         == 0) cmd = "sim/FMS/key_slash";
    else if (strcmp(label, ".")         == 0) cmd = "sim/FMS/key_period";
    else if (strcmp(label, "+/-")       == 0) cmd = "sim/FMS/key_plus_minus";
    else if (strcmp(label, "EXEC")      == 0) cmd = "sim/FMS/key_exec";
    else if (strcmp(label, "INIT REF")  == 0) cmd = "sim/FMS/key_load";
    else if (strcmp(label, "RTE")       == 0) cmd = "sim/FMS/key_route";
    else if (strcmp(label, "CLB")       == 0) cmd = "sim/FMS/key_clb";
    else if (strcmp(label, "CRZ")       == 0) cmd = "sim/FMS/key_crz";
    else if (strcmp(label, "DES")       == 0) cmd = "sim/FMS/key_des";
    else if (strcmp(label, "DIR INTC")  == 0) cmd = "sim/FMS/key_dir_intc";
    else if (strcmp(label, "LEGS")      == 0) cmd = "sim/FMS/key_legs";
    else if (strcmp(label, "DEP ARR")   == 0) cmd = "sim/FMS/key_dep_arr";
    else if (strcmp(label, "HOLD")      == 0) cmd = "sim/FMS/key_hold";
    else if (strcmp(label, "PROG")      == 0) cmd = "sim/FMS/key_prog";
    else if (strcmp(label, "FIX")       == 0) cmd = "sim/FMS/key_fix";
    else if (strcmp(label, "NAV RAD")   == 0) cmd = "sim/FMS/key_nav_rad";
    else if (strcmp(label, "PREV PAGE") == 0) cmd = "sim/FMS/key_prev_page";
    else if (strcmp(label, "NEXT PAGE") == 0) cmd = "sim/FMS/key_next_page";
    else if (strcmp(label, "PERF")      == 0) cmd = "sim/FMS/key_perf";
    if (cmd) xplane_send_command(d->app->xp_send_sock, d->app->xp_host, d->app->xp_send_port, cmd);
}

static void cdu_button_action(FMCData* d, const char* label) {
    xp_forward_cdu_key(d, label);

    if (strcmp(label, "INIT REF") == 0) { d->current_page = 0; d->legs_scroll = 0; return; }
    if (strcmp(label, "RTE")     == 0) { d->current_page = 1; d->legs_scroll = 0; return; }
    if (strcmp(label, "CLB")     == 0) { set_message(d, "CLB: NOT IN USE"); return; }
    if (strcmp(label, "CRZ")     == 0) { set_message(d, "CRZ: NOT IN USE"); return; }
    if (strcmp(label, "DES")     == 0) { set_message(d, "DES: NOT IN USE"); return; }
    if (strcmp(label, "DIR INTC") == 0) { set_message(d, "DIR: SELECT WPT"); return; }
    if (strcmp(label, "LEGS")    == 0) { d->current_page = 2; return; }
    if (strcmp(label, "DEP ARR") == 0) { set_message(d, "DEP/ARR: USE RTE"); return; }
    if (strcmp(label, "HOLD")    == 0) { set_message(d, "HOLD: NOT IN USE"); return; }
    if (strcmp(label, "PROG")    == 0) { d->current_page = 4; return; }
    if (strcmp(label, "FIX")      == 0) { set_message(d, "FIX: NOT IN USE"); return; }
    if (strcmp(label, "NAV RAD")  == 0) { d->current_page = 5; return; }
    if (strcmp(label, "PREV PAGE") == 0) { d->current_page = (d->current_page + 5) % 6; d->legs_scroll = 0; return; }
    if (strcmp(label, "NEXT PAGE") == 0) { d->current_page = (d->current_page + 1) % 6; d->legs_scroll = 0; return; }
    if (strcmp(label, "PERF")    == 0) { d->current_page = 3; return; }
    if (strcmp(label, "EXEC")    == 0) { exec_scratchpad(d); return; }

    if (strcmp(label, "CLR") == 0) { memset(d->scratchpad, 0, sizeof(d->scratchpad)); d->message_scroll = 0; return; }
    if (strcmp(label, "DEL") == 0) { int slen = (int)strlen(d->scratchpad); if (slen > 0) d->scratchpad[slen - 1] = '\0'; return; }
    if (strcmp(label, "SP") == 0) { int slen = (int)strlen(d->scratchpad); if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = ' '; d->scratchpad[slen + 1] = '\0'; } return; }
    if (strcmp(label, "/") == 0) { int slen = (int)strlen(d->scratchpad); if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = '/'; d->scratchpad[slen + 1] = '\0'; } return; }
    if (strcmp(label, "+/-") == 0) { int slen = (int)strlen(d->scratchpad); if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = '-'; d->scratchpad[slen + 1] = '\0'; } return; }
    if (label[0] >= '0' && label[0] <= '9' && label[1] == '\0') { int slen = (int)strlen(d->scratchpad); if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = label[0]; d->scratchpad[slen + 1] = '\0'; } return; }
    if (strcmp(label, ".") == 0) { int slen = (int)strlen(d->scratchpad); if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = '.'; d->scratchpad[slen + 1] = '\0'; } return; }
    if (label[0] >= 'A' && label[0] <= 'Z' && label[1] == '\0') { int slen = (int)strlen(d->scratchpad); if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = label[0]; d->scratchpad[slen + 1] = '\0'; } return; }
}

static void parse_json_layout(StandaloneFMC* sfmc) {
    FILE* f = fopen("location-FMC.json", "r");
    if (!f) return;
    char line[512];
    int in_screen = 0;
    while (fgets(line, sizeof(line), f)) {
        char* label_ptr = strstr(line, "\"label\"");
        char* bbox_ptr = strstr(line, "\"bbox\"");
        
        if (label_ptr && strstr(label_ptr, "DISPLAY_SCREEN")) {
            in_screen = 1;
        } else if (in_screen && bbox_ptr) {
            sscanf(bbox_ptr, "\"bbox\": [%f, %f, %f, %f]", &sfmc->screen_bbox.x1, &sfmc->screen_bbox.y1, &sfmc->screen_bbox.x2, &sfmc->screen_bbox.y2);
            in_screen = 0;
        } else if (label_ptr && bbox_ptr) {
            char label[32] = {0};
            float x1, y1, x2, y2;
            char* val = strchr(label_ptr + 7, '"');
            if (val) {
                val++;
                char* end = strchr(val, '"');
                if (end) {
                    int len = end - val;
                    if (len > 31) len = 31;
                    strncpy(label, val, len);
                }
            }
            sscanf(bbox_ptr, "\"bbox\": [%f, %f, %f, %f]", &x1, &y1, &x2, &y2);
            strcpy(sfmc->buttons[sfmc->button_count].label, label);
            sfmc->buttons[sfmc->button_count].x1 = x1;
            sfmc->buttons[sfmc->button_count].y1 = y1;
            sfmc->buttons[sfmc->button_count].x2 = x2;
            sfmc->buttons[sfmc->button_count].y2 = y2;
            sfmc->button_count++;
        }
    }
    fclose(f);
}

int standalone_fmc_init(App* app) {
    g_sfmc = calloc(1, sizeof(StandaloneFMC));
    if (!g_sfmc) return -1;

    g_sfmc->window = SDL_CreateWindow("Standalone FMC", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 600, 900, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_sfmc->window) { free(g_sfmc); return -1; }

    g_sfmc->renderer = SDL_CreateRenderer(g_sfmc->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_sfmc->renderer) g_sfmc->renderer = SDL_CreateRenderer(g_sfmc->window, -1, SDL_RENDERER_SOFTWARE);

    SDL_Surface* surf = IMG_Load("assets/assets/fmc.png");
    if (surf) {
        g_sfmc->bg_texture = SDL_CreateTextureFromSurface(g_sfmc->renderer, surf);
        g_sfmc->win_w = surf->w;
        g_sfmc->win_h = surf->h;
        SDL_FreeSurface(surf);
        SDL_SetWindowSize(g_sfmc->window, g_sfmc->win_w, g_sfmc->win_h);
    } else {
        g_sfmc->win_w = 600;
        g_sfmc->win_h = 900;
        LOG_WARN("Failed to load assets/assets/fmc.png: %s", IMG_GetError());
    }

    parse_json_layout(g_sfmc);

    g_sfmc->fmc_data = calloc(1, sizeof(FMCData));
    FMCData* d = g_sfmc->fmc_data;
    d->fmc = app->fmc_state;
    d->app = app;
    d->current_page = 0;
    d->message_history = ll_create();
    if (d->fmc) d->graph = route_graph_build(d->fmc);
    d->wpt_tree = avl_create(wpt_compare_ident, NULL);

    build_page(d);
    return 0;
}

void standalone_fmc_update(const FlightData* fd, float dt) {
    if (!g_sfmc) return;
    FMCData* d = g_sfmc->fmc_data;
    if (fd) {
        FlightDataValues snapshot;
        flight_data_snapshot((FlightData*)fd, &snapshot);
        d->smooth_gs = exp_smooth(d->smooth_gs, snapshot.gs_kts, 0.1f);
        d->smooth_alt = exp_smooth(d->smooth_alt, snapshot.altitude_ft, 0.1f);
    }
    build_page(d);
}

void standalone_fmc_render(void) {
    if (!g_sfmc) return;

    SDL_SetRenderDrawColor(g_sfmc->renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sfmc->renderer);

    int win_w, win_h;
    SDL_GetWindowSize(g_sfmc->window, &win_w, &win_h);

    if (g_sfmc->bg_texture) {
        SDL_Rect dest = {0, 0, win_w, win_h};
        SDL_RenderCopy(g_sfmc->renderer, g_sfmc->bg_texture, NULL, &dest);
    }

    FMCData* d = g_sfmc->fmc_data;
    int sx = (int)(g_sfmc->screen_bbox.x1 * win_w);
    int sy = (int)(g_sfmc->screen_bbox.y1 * win_h);
    int sw = (int)((g_sfmc->screen_bbox.x2 - g_sfmc->screen_bbox.x1) * win_w);
    int sh = (int)((g_sfmc->screen_bbox.y2 - g_sfmc->screen_bbox.y1) * win_h);

    int line_h = sh / 14;
    float scale = (float)sh / 400.0f; 

    /* Display lines */
    for (int ln = 0; ln < 12; ln++) {
        int y = sy + ln * line_h + line_h / 2;
        set_col(g_sfmc->renderer, COL_WHITE);
        font_draw_scaled_aligned(g_sfmc->renderer, sx + 4, y, d->display[ln], 0.6f * scale, FONT_MONO, FONT_ALIGN_LEFT);
    }

    /* Scratchpad */
    int scratch_line_y = sy + sh - line_h * 2;
    set_col(g_sfmc->renderer, COL_GRAY);
    font_draw_scaled_aligned(g_sfmc->renderer, sx + 4, scratch_line_y + line_h / 2, "SCRATCHPAD", 0.5f * scale, FONT_REGULAR, FONT_ALIGN_LEFT);

    char sp_display[32];
    int blink = ((SDL_GetTicks() / 500) % 2);
    snprintf(sp_display, sizeof(sp_display), "%s%c", d->scratchpad, blink ? '_' : ' ');
    set_col(g_sfmc->renderer, COL_CYAN);
    font_draw_scaled_aligned(g_sfmc->renderer, sx + 4, scratch_line_y + line_h + line_h / 2, sp_display, 0.7f * scale, FONT_MONO, FONT_ALIGN_LEFT);

    /* Messages */
    int msg_y = scratch_line_y + line_h + 4;
    int total_msgs = d->message_history ? ll_size(d->message_history) : 0;
    if (total_msgs > 0) {
        int idx = total_msgs - 1 - d->message_scroll;
        if (idx < 0) idx = 0;
        FmcMessage* m = (FmcMessage*)ll_get(d->message_history, idx);
        if (m) {
            int show = (d->message_scroll > 0) || ((SDL_GetTicks() - m->timestamp) < MSG_TIMEOUT_MS);
            if (show) {
                set_col(g_sfmc->renderer, COL_AMBER);
                font_draw_scaled_aligned(g_sfmc->renderer, sx + sw / 2, msg_y, m->text, 0.6f * scale, FONT_REGULAR, FONT_ALIGN_CENTER);
            }
        }
    }

    SDL_RenderPresent(g_sfmc->renderer);
}

int standalone_fmc_event(const SDL_Event* ev) {
    if (!g_sfmc) return 0;

    if (ev->type == SDL_WINDOWEVENT && ev->window.windowID == SDL_GetWindowID(g_sfmc->window)) {
        return 1;
    }

    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.windowID == SDL_GetWindowID(g_sfmc->window)) {
        int win_w, win_h;
        SDL_GetWindowSize(g_sfmc->window, &win_w, &win_h);
        int mx = ev->button.x;
        int my = ev->button.y;

        for (int i = 0; i < g_sfmc->button_count; i++) {
            float x1 = g_sfmc->buttons[i].x1 * win_w;
            float y1 = g_sfmc->buttons[i].y1 * win_h;
            float x2 = g_sfmc->buttons[i].x2 * win_w;
            float y2 = g_sfmc->buttons[i].y2 * win_h;

            if (mx >= x1 && mx <= x2 && my >= y1 && my <= y2) {
                const char* lbl = g_sfmc->buttons[i].label;
                FMCData* d = g_sfmc->fmc_data;
                if (strncmp(lbl, "LSK_", 4) == 0) {
                    int line = lbl[4] - '1';
                    int side = (lbl[5] == 'L') ? 0 : 1;
                    handle_lsk(d, side, line);
                } else if (strcmp(lbl, "PLUS_MINUS") == 0) cdu_button_action(d, "+/-");
                else if (strcmp(lbl, "DOT") == 0) cdu_button_action(d, ".");
                else if (strcmp(lbl, "SPACE") == 0) cdu_button_action(d, "SP");
                else if (strcmp(lbl, "SLASH") == 0) cdu_button_action(d, "/");
                else if (strcmp(lbl, "INIT_REF") == 0) cdu_button_action(d, "INIT REF");
                else if (strcmp(lbl, "DIR_INTC") == 0) cdu_button_action(d, "DIR INTC");
                else if (strcmp(lbl, "DEP_ARR") == 0) cdu_button_action(d, "DEP ARR");
                else if (strcmp(lbl, "NAV_RAD") == 0) cdu_button_action(d, "NAV RAD");
                else if (strcmp(lbl, "PREV_PAGE") == 0) cdu_button_action(d, "PREV PAGE");
                else if (strcmp(lbl, "NEXT_PAGE") == 0) cdu_button_action(d, "NEXT PAGE");
                else if (strncmp(lbl, "NUM_", 4) == 0) { char num[2] = {lbl[4], '\0'}; cdu_button_action(d, num); }
                else cdu_button_action(d, lbl);
                return 1;
            }
        }
        return 1; // Click in FMC window but not on a button
    }

    if (ev->type == SDL_KEYDOWN && ev->key.windowID == SDL_GetWindowID(g_sfmc->window)) {
        FMCData* d = g_sfmc->fmc_data;
        SDL_Keycode key = ev->key.keysym.sym;

        if (key == SDLK_F1) { d->current_page = 0; d->legs_scroll = 0; return 1; }
        if (key == SDLK_F2) { d->current_page = 1; d->legs_scroll = 0; return 1; }
        if (key == SDLK_F3) { d->current_page = 2; return 1; }
        if (key == SDLK_F4) { d->current_page = 3; return 1; }
        if (key == SDLK_F5) { d->current_page = 4; return 1; }
        if (key == SDLK_F7) { d->current_page = 5; return 1; }

        if (key == SDLK_F6) {
            d->legs_sort_mode = !d->legs_sort_mode;
            d->legs_scroll = 0;
            set_message(d, d->legs_sort_mode ? "LEGS: ALPHABETICAL" : "LEGS: ROUTE ORDER");
            return 1;
        }

        if (key == SDLK_TAB) {
            d->current_page = (d->current_page + 1) % 6;
            d->legs_scroll  = 0;
            return 1;
        }

        if (key == SDLK_PAGEDOWN && d->current_page == 2) {
            FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;
            if (fp && fp->waypoint_count > 10) {
                d->legs_scroll += 10;
                if (d->legs_scroll >= fp->waypoint_count)
                    d->legs_scroll = fp->waypoint_count - 10;
                if (d->legs_scroll < 0) d->legs_scroll = 0;
            }
            return 1;
        }
        if (key == SDLK_PAGEUP && d->current_page == 2) {
            d->legs_scroll -= 10;
            if (d->legs_scroll < 0) d->legs_scroll = 0;
            return 1;
        }

        if (key == SDLK_PAGEUP && d->current_page != 2) {
            int total = ll_size(d->message_history);
            if (total > 1 && d->message_scroll < total - 1) {
                d->message_scroll++;
            }
            return 1;
        }
        if (key == SDLK_PAGEDOWN && d->current_page != 2) {
            if (d->message_scroll > 0) {
                d->message_scroll--;
            }
            return 1;
        }

        int slen = (int)strlen(d->scratchpad);

        if (key == SDLK_BACKSPACE || key == SDLK_DELETE) {
            if (slen > 0) d->scratchpad[slen - 1] = '\0';
            xp_forward_cdu_key(d, "DEL");
            return 1;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            exec_scratchpad(d);
            xp_forward_cdu_key(d, "EXEC");
            return 1;
        }
        if (key == SDLK_ESCAPE) {
            memset(d->scratchpad, 0, sizeof(d->scratchpad));
            d->message_scroll = 0;
            xp_forward_cdu_key(d, "CLR");
            return 1;
        }

        char ch = '\0';
        if (key >= SDLK_a && key <= SDLK_z)       ch = (char)('A' + (key - SDLK_a));
        else if (key >= SDLK_0 && key <= SDLK_9)   ch = (char)('0' + (key - SDLK_0));
        else if (key == SDLK_SPACE)                ch = ' ';
        else if (key == SDLK_PERIOD)               ch = '.';
        else if (key == SDLK_SLASH)                ch = '/';
        else if (key == SDLK_MINUS)                ch = '-';

        if (ch && slen < MAX_SCRATCHPAD) {
            d->scratchpad[slen]     = ch;
            d->scratchpad[slen + 1] = '\0';
            char lbl[2] = {ch, '\0'};
            if (ch == ' ')      xp_forward_cdu_key(d, "SP");
            else if (ch == '/') xp_forward_cdu_key(d, "/");
            else if (ch == '.') xp_forward_cdu_key(d, ".");
            else if (ch == '-') xp_forward_cdu_key(d, "+/-");
            else                xp_forward_cdu_key(d, lbl);
            return 1;
        }
    }
    return 0;
}

void standalone_fmc_destroy(void) {
    if (!g_sfmc) return;
    if (g_sfmc->fmc_data) {
        if (g_sfmc->fmc_data->graph) route_graph_destroy(g_sfmc->fmc_data->graph);
        if (g_sfmc->fmc_data->wpt_tree) avl_destroy(g_sfmc->fmc_data->wpt_tree, 0);
        if (g_sfmc->fmc_data->message_history) ll_destroy(g_sfmc->fmc_data->message_history, 1);
        free(g_sfmc->fmc_data);
    }
    if (g_sfmc->bg_texture) SDL_DestroyTexture(g_sfmc->bg_texture);
    if (g_sfmc->renderer) SDL_DestroyRenderer(g_sfmc->renderer);
    if (g_sfmc->window) SDL_DestroyWindow(g_sfmc->window);
    free(g_sfmc);
    g_sfmc = NULL;
}