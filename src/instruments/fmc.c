/**
 * @file    fmc.c
 * @brief   FMC / CDU instrument — full interactive implementation.
 *
 * Display pages:
 *   0 — IDENT:    Aircraft/engine model, nav data info
 *   1 — RTE:      Route entry (ORIGIN / DEST / FLT NO / ACTIVATE)
 *   2 — LEGS:     Waypoint list with distances, scrollable
 *   3 — PERF:     Performance (cruise alt, speed, fuel)
 *   4 — PROG:     Progress (DTG, ETA, fuel, GS, ALT)
 *
 * Interaction:
 *   - Keyboard:  F1-F5 = page nav, alphanumeric = scratchpad, Enter = EXEC
 *   - Mouse:     Click LSK markers to assign scratchpad → field or trigger action
 *   - RTE page:  Type ICAO → click LSK → ACTIVATE runs A* search
 */

#include "fmc.h"
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

/* =========================================================================
 *  Colors
 * ========================================================================= */

#define COL_BG         0x05, 0x05, 0x0A, 255
#define COL_SCREEN_BG  0x0A, 0x0A, 0x15, 255
#define COL_WHITE      0xFF, 0xFF, 0xFF, 255
#define COL_GREEN      0x00, 0xFF, 0x40, 255
#define COL_CYAN       0x00, 0xFF, 0xFF, 255
#define COL_AMBER      0xFF, 0xC0, 0x00, 255
#define COL_ORANGE     0xFF, 0x88, 0x00, 255
#define COL_GRAY       0x60, 0x60, 0x68, 255
#define COL_DARK_GRAY  0x30, 0x30, 0x38, 255
#define COL_TAPE_BG    0x20, 0x20, 0x28, 255
#define COL_HILITE     0x00, 0x80, 0xFF, 255  /* Blue highlight for active field */

/* =========================================================================
 *  Constants
 * ========================================================================= */

#define MAX_SCRATCHPAD  24
#define MSG_TIMEOUT_MS  4000
#define MSG_MAX_HISTORY 50

/* CDU layout — MUST be consistent between draw and hit-test */
#define SCREEN_MARGIN   10
#define TITLE_BAR_H     20
#define SCREEN_H_PCT    52    /* screen height as % of instrument height */
#define KB_GAP          8     /* gap between screen bottom and keypad top */
#define KB_BOTTOM_GAP   4     /* gap from keypad bottom to instrument edge */
#define LSK_BTN_W       36    /* width of each LSK button */
#define LSK_GAP         2     /* gap between LSK button and screen edge */

/* =========================================================================
 *  Message history node (stored in linked list)
 * ========================================================================= */

typedef struct {
    char   text[64];
    Uint32 timestamp;
} FmcMessage;

/* =========================================================================
 *  FMC private data
 * ========================================================================= */

typedef struct {
    /* Display state */
    int    current_page;           /* 0=IDENT, 1=RTE, 2=LEGS, 3=PERF, 4=PROG */
    char   scratchpad[MAX_SCRATCHPAD + 1];

    /* FMC shared state (owned by App) */
    FMCState* fmc;

    /* App pointer (for X-Plane DREF sync) */
    App*    app;

    /* Route graph (built once from nav data) */
    RouteGraph* graph;

    /* RTE page: temporary input buffers for origin/dest */
    char   origin_input[8];
    char   dest_input[8];

    /* LEGS page scroll + sort */
    int    legs_scroll;            /* First visible waypoint index */
    int    legs_sort_mode;         /* 0 = route order, 1 = alphabetical */

    /* AVL tree for sorted waypoint display */
    AVLTree* wpt_tree;

    /* Message history (linked list of FmcMessage) */
    LinkedList* message_history;
    int         message_scroll;    /* 0 = latest, 1+ = older messages */

    /* Smoothing */
    float  smooth_gs;
    float  smooth_alt;

    /* Page display buffer (12 lines × 24 chars + null) */
    char   display[12][25];

    /* Cached screen layout for mouse hit-testing */
    SDL_Rect screen_area;          /* Screen background rect */
    int      line_h;               /* Height of one display line */

    /* Clickable CDU keypad */
    int      kb_y;                 /* Top of keyboard area */
    int      kb_h;                 /* Keyboard area height */
    
    /* Background and layout */
    SDL_Texture* bg_texture;
    struct {
        float x1, y1, x2, y2;
    } screen_bbox;
    struct {
        char label[32];
        float x1, y1, x2, y2;
    } buttons[100];
    int button_count;
} FMCData;

/* =========================================================================
 *  Forward declarations
 * ========================================================================= */

static void build_page(FMCData* d);
static void rebuild_lsk_labels(FMCData* d);
static void set_message(FMCData* d, const char* msg);
static void exec_scratchpad(FMCData* d);
static void handle_lsk(FMCData* d, int side, int line);
static void rte_activate(FMCData* d);

/* =========================================================================
 *  Color helper
 * ========================================================================= */

static void set_col(SDL_Renderer* r, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    SDL_SetRenderDrawColor(r, R, G, B, A);
}

/* =========================================================================
 *  Message system
 * ========================================================================= */

static void parse_json_layout(FMCData* d) {
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
            sscanf(bbox_ptr, "\"bbox\": [%f, %f, %f, %f]", &d->screen_bbox.x1, &d->screen_bbox.y1, &d->screen_bbox.x2, &d->screen_bbox.y2);
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
            strcpy(d->buttons[d->button_count].label, label);
            d->buttons[d->button_count].x1 = x1;
            d->buttons[d->button_count].y1 = y1;
            d->buttons[d->button_count].x2 = x2;
            d->buttons[d->button_count].y2 = y2;
            d->button_count++;
        }
    }
    fclose(f);
}

static void set_message(FMCData* d, const char* msg)
{
    if (!d->message_history) return;

    FmcMessage* m = calloc(1, sizeof(FmcMessage));
    if (!m) return;
    strncpy(m->text, msg, sizeof(m->text) - 1);
    m->text[sizeof(m->text) - 1] = '\0';
    m->timestamp = SDL_GetTicks();

    ll_push_back(d->message_history, m);

    /* Cap history at MSG_MAX_HISTORY */
    while (ll_size(d->message_history) > MSG_MAX_HISTORY) {
        FmcMessage* old = (FmcMessage*)ll_pop_front(d->message_history);
        free(old);
    }

    /* Always show latest message */
    d->message_scroll = 0;
}

/* =========================================================================
 *  LSK label system
 * ========================================================================= */

/* =========================================================================
 *  Page builders
 * ========================================================================= */

static void build_ident_page(FMCData* d)
{
    memset(d->display, ' ', sizeof(d->display));

    snprintf(d->display[0], 25, " FMC-CDU  VER 2.0      ");
    snprintf(d->display[2], 25, " MODEL   B738          ");
    snprintf(d->display[3], 25, " ENG     CFM56-7B26    ");
    snprintf(d->display[5], 25, " NAV DATA              ");
    snprintf(d->display[6], 25, " %04d WAYPOINTS         ",
             d->fmc ? d->fmc->nav_wpt_count : 0);
    snprintf(d->display[7], 25, " %04d AIRPORTS          ",
             d->fmc ? d->fmc->nav_apt_count : 0);
    snprintf(d->display[8], 25, " %04d GRAPH NODES       ",
             d->graph ? d->graph->node_count : 0);
    snprintf(d->display[10], 25, " <IDENT               ");

    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static void build_rte_page(FMCData* d)
{
    memset(d->display, ' ', sizeof(d->display));

    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;

    snprintf(d->display[0], 25, " RTE            1/5    ");

    /* L1: ORIGIN */
    snprintf(d->display[1], 25, " ORIGIN  %-8s     <L1  ",
             (fp && fp->departure.icao[0]) ? fp->departure.icao : "----");
    /* R1: DEST */
    snprintf(d->display[2], 25, " DEST    %-8s      R1> ",
             (fp && fp->arrival.icao[0]) ? fp->arrival.icao : "----");
    /* L2: FLT NO */
    snprintf(d->display[3], 25, " FLT NO  %-8s     <L2  ",
             (fp && fp->flight_number[0]) ? fp->flight_number : "----");
    /* R2: ALT (informational — shows on PERF page) */
    snprintf(d->display[4], 25, " CRZ ALT %05.0f FT  R2> ",
             fp ? (double)fp->cruise_altitude_ft : 35000.0);

    /* Stats */
    snprintf(d->display[6], 25, " WPT: %03d  DIST: %6.0f  ",
             fp ? fp->waypoint_count : 0,
             fp ? (double)fp->total_distance_nm : 0.0);
    snprintf(d->display[7], 25, " ETE: %4.1f HR          ",
             fp ? (double)fp->estimated_time_hours : 0.0);

    /* L6: RTE page nav, R6: ACTIVATE */
    snprintf(d->display[10], 25, " <RTE                  ");
    snprintf(d->display[11], 25, "                ACTIV> ");

    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

/* --- AVL comparator: sort waypoints by ident --- */
static int wpt_compare_ident(const void* a, const void* b, void* userdata)
{
    (void)userdata;
    return strcmp(((const Waypoint*)a)->ident, ((const Waypoint*)b)->ident);
}

/* --- Rebuild AVL tree from flight plan waypoints --- */
static void rebuild_wpt_tree(FMCData* d)
{
    if (!d->wpt_tree) return;
    /* Clear old tree (don't free data — waypoints owned by flight plan) */
    AVLTree* old = d->wpt_tree;
    d->wpt_tree = avl_create(wpt_compare_ident, NULL);
    avl_destroy(old, 0);

    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;
    if (!fp || fp->waypoint_count == 0) return;

    for (int i = 0; i < fp->waypoint_count; i++) {
        avl_insert(d->wpt_tree, &fp->waypoints[i]);
    }
}

/* --- Inorder collector for AVL → temp array --- */
typedef struct { const Waypoint* wpts[128]; int count; } WptCollector;

static int collect_wpt(void* data, void* userdata)
{
    WptCollector* c = (WptCollector*)userdata;
    if (c->count < 128) c->wpts[c->count++] = (const Waypoint*)data;
    return 0; /* continue traversal */
}

static void build_legs_page(FMCData* d)
{
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
        /* Alphabetical order via AVL tree */
        rebuild_wpt_tree(d);
        WptCollector col = {{NULL}, 0};
        avl_inorder(d->wpt_tree, collect_wpt, &col);

        int max_show = 10;
        int start = d->legs_scroll;
        if (start > col.count - max_show) start = col.count - max_show;
        if (start < 0) start = 0;

        for (int i = 0; i < max_show && (start + i) < col.count; i++) {
            const Waypoint* w = col.wpts[start + i];
            int is_active = (fp->active_waypoint_index >= 0 &&
                &fp->waypoints[fp->active_waypoint_index] == w);
            snprintf(d->display[i + 1], 25, "%c%-8s              ",
                     is_active ? '>' : ' ', w->ident[0] ? w->ident : "......");
        }
    } else {
        /* Route order — iterate array directly */
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
                cum_dist += (float)geo_distance_nm(fp->waypoints[j - 1].pos,
                                                   fp->waypoints[j].pos);
            }

            snprintf(d->display[i + 1], 25, "%c%-7s %6.0f NM      ",
                     is_active ? '>' : ' ',
                     w->ident[0] ? w->ident : "......",
                     (double)cum_dist);
        }
    }

    /* Navigation hints */
    if (count > 10) {
        snprintf(d->display[11], 25, " <PG UP  PG DN  SORT> ");
    } else {
        snprintf(d->display[11], 25, " <LEGS          SORT> ");
    }

    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static void build_perf_page(FMCData* d)
{
    memset(d->display, ' ', sizeof(d->display));

    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;

    snprintf(d->display[0], 25, " PERF INIT       3/5    ");
    snprintf(d->display[1], 25, " CRZ ALT %05.0f FT      ",
             fp ? (double)fp->cruise_altitude_ft : 35000.0);
    snprintf(d->display[2], 25, " CRZ SPD %03.0f KT      ",
             fp ? (double)fp->cruise_speed_kts : 450.0);
    snprintf(d->display[3], 25, " FUEL REQ %05.0f LBS    ",
             fp ? (double)fp->fuel_required_lbs : 0.0);
    snprintf(d->display[4], 25, " TOT DIST %.0f NM       ",
             fp ? (double)fp->total_distance_nm : 0.0);
    snprintf(d->display[5], 25, " CI      80             ");
    snprintf(d->display[6], 25, " GW      140000 LBS     ");
    snprintf(d->display[7], 25, " RESV    5000 LBS       ");

    if (d->graph) {
        snprintf(d->display[9], 25, " GRAPH: %d NODES       ",
                 d->graph->node_count);
    }

    snprintf(d->display[11], 25, " <PERF                 ");

    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static void build_prog_page(FMCData* d)
{
    memset(d->display, ' ', sizeof(d->display));

    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;

    snprintf(d->display[0], 25, " PROGRESS        4/5    ");

    /* Distance To Go */
    {
        float dtg = fp ? fp->total_distance_nm : 0.0f;
        int active = fp ? fp->active_waypoint_index : -1;
        if (active >= 0 && active < fp->waypoint_count && fp->waypoint_count > 0) {
            dtg = 0.0f;
            for (int j = active; j < fp->waypoint_count - 1; j++) {
                dtg += (float)geo_distance_nm(fp->waypoints[j].pos,
                                              fp->waypoints[j + 1].pos);
            }
        }
        snprintf(d->display[2], 25, " DTG    %.0f NM         ", (double)dtg);
    }

    snprintf(d->display[3], 25, " ETA    %4.1f HR        ",
             fp ? (double)fp->estimated_time_hours : 0.0);
    snprintf(d->display[4], 25, " FUEL   %.0f LBS        ",
             fp ? (double)fp->fuel_required_lbs : 0.0);
    snprintf(d->display[6], 25, " GS     %.0f KT         ",
             (double)d->smooth_gs);
    snprintf(d->display[7], 25, " ALT    %.0f FT         ",
             (double)d->smooth_alt);
    snprintf(d->display[9], 25, " WPT    %d/%d           ",
             fp ? fp->active_waypoint_index + 1 : 0,
             fp ? fp->waypoint_count : 0);

    snprintf(d->display[11], 25, " <PROG                 ");

    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static void build_radio_page(FMCData* d)
{
    memset(d->display, ' ', sizeof(d->display));

    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;
    (void)fp;

    snprintf(d->display[0], 25, " RADIO            6/6    ");

    /* COM1 */
    snprintf(d->display[2], 25, " COM1  %06.3f          ",
             d->smooth_gs > 0.0f ? 122.800 : 122.800);
    /* COM2 */
    snprintf(d->display[3], 25, " COM2  %06.3f          ",
             121.500);
    /* NAV1 */
    snprintf(d->display[5], 25, " NAV1  %06.2f          ",
             110.90);
    /* NAV2 */
    snprintf(d->display[6], 25, " NAV2  %06.2f          ",
             113.70);
    /* XPDR */
    snprintf(d->display[8], 25, " XPDR  1200 ALT        ");
    /* DME */
    snprintf(d->display[9], 25, " DME   ---- NM         ");

    snprintf(d->display[11], 25, " <RADIO                ");

    for (int i = 0; i < 12; i++) d->display[i][24] = '\0';
}

static void build_page(FMCData* d)
{
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

/* =========================================================================
 *  Route activation (A* search)
 * ========================================================================= */

static void rte_activate(FMCData* d)
{
    if (!d->fmc || !d->graph) {
        set_message(d, "ERR: NO NAV DATA");
        LOG_WARN("FMC: cannot activate — no fmc or graph");
        return;
    }

    FlightPlan* fp = &d->fmc->flight_plan;

    /* Determine origin and dest */
    const char* orig = (fp->departure.icao[0]) ? fp->departure.icao : NULL;
    const char* dest = (fp->arrival.icao[0])   ? fp->arrival.icao   : NULL;

    if (!orig || !dest) {
        set_message(d, "ENTER ORIGIN AND DEST");
        return;
    }

    /* Verify both airports exist in the graph */
    if (fp->departure.pos.lat_deg == 0.0 && fp->departure.pos.lon_deg == 0.0) {
        /* Search nav_airports for the origin */
        int found = 0;
        for (int i = 0; i < d->fmc->nav_apt_count; i++) {
            if (strcmp(d->fmc->nav_airports[i].icao, orig) == 0) {
                fp->departure = d->fmc->nav_airports[i];
                found = 1;
                break;
            }
        }
        if (!found) {
            set_message(d, "ORIGIN NOT IN DATABASE");
            return;
        }
    }

    if (fp->arrival.pos.lat_deg == 0.0 && fp->arrival.pos.lon_deg == 0.0) {
        int found = 0;
        for (int i = 0; i < d->fmc->nav_apt_count; i++) {
            if (strcmp(d->fmc->nav_airports[i].icao, dest) == 0) {
                fp->arrival = d->fmc->nav_airports[i];
                found = 1;
                break;
            }
        }
        if (!found) {
            set_message(d, "DEST NOT IN DATABASE");
            return;
        }
    }

    /* Set defaults */
    if (fp->cruise_altitude_ft < 100.0f) fp->cruise_altitude_ft = 35000.0f;
    if (fp->cruise_speed_kts < 50.0f)    fp->cruise_speed_kts    = 450.0f;

    set_message(d, "COMPUTING ROUTE...");

    /* Rebuild graph (in case nav data changed) */
    /* Actually we build once at init. Let's just search. */

    /* Run A* */
    RoutePath path;
    int found = route_graph_find_route(d->graph, orig, dest,
                                       fp->cruise_altitude_ft, &path);

    if (!found || path.waypoint_count < 2) {
        set_message(d, "NO ROUTE FOUND");
        return;
    }

    /* Populate flight plan */
    flight_plan_clear(fp);

    /* Set departure/arrival */
    strncpy(fp->departure.icao, orig, sizeof(fp->departure.icao) - 1);
    strncpy(fp->arrival.icao, dest, sizeof(fp->arrival.icao) - 1);

    /* Copy waypoints */
    for (int i = 0; i < path.waypoint_count && i < MAX_ROUTE_WAYPOINTS; i++) {
        fp->waypoints[i] = path.waypoints[i];
        fp->waypoint_count = i + 1;
    }

    fp->total_distance_nm   = path.total_distance_nm;
    fp->estimated_time_hours = path.estimated_time_hours;
    fp->fuel_required_lbs    = path.estimated_time_hours * 5000.0f; /* rough estimate */
    fp->active_waypoint_index = 0;
    d->fmc->plan_modified     = 1;

    d->legs_scroll = 0;

    {
        char buf[64];
        snprintf(buf, sizeof(buf), "ROUTE OK %d WP %.0f NM",
                 path.waypoint_count, (double)path.total_distance_nm);
        set_message(d, buf);
    }

    LOG_INFO("FMC: route activated %s→%s, %d WP, %.0f NM",
             orig, dest, path.waypoint_count, (double)path.total_distance_nm);

    /* Sync flight plan to X-Plane autopilot via native DREF */
    if (d->app) {
        xplane_fmc_sync(d->app);
    }
}

/* =========================================================================
 *  Scratchpad execution
 * ========================================================================= */

static void exec_scratchpad(FMCData* d)
{
    if (d->scratchpad[0] == '\0') return;

    char cmd[MAX_SCRATCHPAD + 1];
    strncpy(cmd, d->scratchpad, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    /* Convert to uppercase */
    for (char* p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);

    /* Page-specific actions */
    switch (d->current_page) {
    case 1: /* RTE — assume it's an airport ICAO, try assigning */
        {
            FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;
            if (!fp) break;

            /* If origin empty, assign to origin; else assign to dest */
            if (fp->departure.icao[0] == '\0') {
                strncpy(fp->departure.icao, cmd, sizeof(fp->departure.icao) - 1);
                set_message(d, "ORIGIN SET");
            } else if (fp->arrival.icao[0] == '\0') {
                strncpy(fp->arrival.icao, cmd, sizeof(fp->arrival.icao) - 1);
                set_message(d, "DEST SET");
            } else {
                /* Both set — treat as ACTIVATE */
                rte_activate(d);
            }
        }
        break;

    case 3: /* PERF — parse cruise alt */
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

    /* Clear scratchpad */
    memset(d->scratchpad, 0, sizeof(d->scratchpad));
}

/* =========================================================================
 *  LSK click handling
 * ========================================================================= */

/**
 * @brief Handle a line-select key press.
 * @param side  0 = left LSK, 1 = right LSK
 * @param line  0-5 (top to bottom, corresponding to display rows 0/2/4/6/8/10)
 */
static void handle_lsk(FMCData* d, int side, int line)
{
    /* Mirror LSK press to X-Plane */
    if (d->app && d->app->xp_send_sock && d->app->xp_host[0]) {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "sim/FMS/key_%c%d",
                 (side == 0) ? 'L' : 'R', line + 1);
        xplane_send_command(d->app->xp_send_sock, d->app->xp_host,
                            d->app->xp_send_port, cmd);
    }

    FlightPlan* fp = d->fmc ? &d->fmc->flight_plan : NULL;

    switch (d->current_page) {
    case 0: /* IDENT */
        if (side == 0 && line == 5) { d->current_page = 0; } /* <IDENT — stay */
        break;

    case 1: /* RTE */
        if (side == 0) {
            switch (line) {
            case 0: /* LSK L1: ORIGIN ← scratchpad */
                if (d->scratchpad[0] && fp) {
                    for (char* p = d->scratchpad; *p; p++)
                        *p = (char)toupper((unsigned char)*p);
                    strncpy(fp->departure.icao, d->scratchpad,
                            sizeof(fp->departure.icao) - 1);
                    set_message(d, "ORIGIN SET");
                    memset(d->scratchpad, 0, sizeof(d->scratchpad));
                } else if (fp) {
                    /* Copy current origin to scratchpad */
                    strncpy(d->scratchpad, fp->departure.icao,
                            sizeof(d->scratchpad) - 1);
                }
                break;
            case 1: /* LSK L2: FLT NO */
                if (d->scratchpad[0] && fp) {
                    for (char* p = d->scratchpad; *p; p++)
                        *p = (char)toupper((unsigned char)*p);
                    strncpy(fp->flight_number, d->scratchpad,
                            sizeof(fp->flight_number) - 1);
                    set_message(d, "FLT NO SET");
                    memset(d->scratchpad, 0, sizeof(d->scratchpad));
                } else if (fp && fp->flight_number[0]) {
                    strncpy(d->scratchpad, fp->flight_number,
                            sizeof(d->scratchpad) - 1);
                }
                break;
            case 5: /* LSK L6: page nav */
                d->current_page = 0; /* back to IDENT */
                break;
            }
        } else { /* side == 1 (right LSK) */
            switch (line) {
            case 0: /* LSK R1: DEST ← scratchpad */
                if (d->scratchpad[0] && fp) {
                    for (char* p = d->scratchpad; *p; p++)
                        *p = (char)toupper((unsigned char)*p);
                    strncpy(fp->arrival.icao, d->scratchpad,
                            sizeof(fp->arrival.icao) - 1);
                    set_message(d, "DEST SET");
                    memset(d->scratchpad, 0, sizeof(d->scratchpad));
                } else if (fp) {
                    strncpy(d->scratchpad, fp->arrival.icao,
                            sizeof(d->scratchpad) - 1);
                }
                break;
            case 1: /* LSK R2: CRZ ALT */
                if (d->scratchpad[0] && fp) {
                    int alt = atoi(d->scratchpad);
                    if (alt > 0) {
                        fp->cruise_altitude_ft = (float)alt;
                        set_message(d, "CRZ ALT SET");
                    }
                    memset(d->scratchpad, 0, sizeof(d->scratchpad));
                } else if (fp) {
                    snprintf(d->scratchpad, sizeof(d->scratchpad),
                             "%.0f", (double)fp->cruise_altitude_ft);
                }
                break;
            case 5: /* LSK R6: ACTIVATE */
                rte_activate(d);
                break;
            }
        }
        break;

    case 2: /* LEGS */
        if (side == 0 && line == 5) {
            d->current_page = 1; /* back to RTE */
        }
        if (side == 1 && line == 5) {
            /* PAGE DOWN */
            if (fp && fp->waypoint_count > 10) {
                d->legs_scroll += 10;
                if (d->legs_scroll >= fp->waypoint_count) {
                    d->legs_scroll = 0; /* wrap around */
                }
            }
        }
        break;

    case 3: /* PERF */
        if (side == 0 && line == 5) {
            d->current_page = 2; /* back to LEGS */
        }
        break;

    case 4: /* PROG */
        if (side == 0 && line == 5) {
            d->current_page = 3; /* back to PERF */
        }
        break;
    }
}

/* =========================================================================
 *  CDU keypad click detection — maps mouse click to button action
 * ========================================================================= */

/* =========================================================================
 *  X-Plane CDU key mirror: label → X-Plane FMS command
 * ========================================================================= */

/**
 * @brief Map a CDU button label to the X-Plane FMS command path,
 *        and send it via native UDP CMND to mirror the keypress.
 */
static void xp_forward_cdu_key(FMCData* d, const char* label)
{
    if (!d || !d->app || !d->app->xp_send_sock) return;
    if (d->app->xp_host[0] == '\0') return;

    const char* cmd = NULL;

    /* --- Single letters: A-Z --- */
    if (label[0] >= 'A' && label[0] <= 'Z' && label[1] == '\0') {
        /* We need to build the command dynamically.
         * "sim/FMS/key_A" is 14 chars + null = 15. */
        char buf[32];
        snprintf(buf, sizeof(buf), "sim/FMS/key_%c", label[0]);
        xplane_send_command(d->app->xp_send_sock, d->app->xp_host,
                            d->app->xp_send_port, buf);
        return;
    }

    /* --- Single digits: 0-9 --- */
    if (label[0] >= '0' && label[0] <= '9' && label[1] == '\0') {
        char buf[32];
        snprintf(buf, sizeof(buf), "sim/FMS/key_%c", label[0]);
        xplane_send_command(d->app->xp_send_sock, d->app->xp_host,
                            d->app->xp_send_port, buf);
        return;
    }

    /* --- Fixed-label mappings --- */
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

    if (cmd) {
        xplane_send_command(d->app->xp_send_sock, d->app->xp_host,
                            d->app->xp_send_port, cmd);
    }
}

/* =========================================================================
 *  CDU keypad click → button action
 * ========================================================================= */

static int cdu_button_action(FMCData* d, const char* label)
{
    /* Mirror every keypress to X-Plane's CDU via native UDP CMND */
    xp_forward_cdu_key(d, label);

    /* === Function keys (3 rows × 5 cols + EXEC) === */

    /* Row 0: INIT REF | RTE | CLB | CRZ | DES */
    if (strcmp(label, "INIT REF") == 0) { d->current_page = 0; d->legs_scroll = 0; return 1; }
    if (strcmp(label, "RTE")     == 0) { d->current_page = 1; d->legs_scroll = 0; return 1; }
    if (strcmp(label, "CLB")     == 0) { set_message(d, "CLB: NOT IN USE"); return 1; }
    if (strcmp(label, "CRZ")     == 0) { set_message(d, "CRZ: NOT IN USE"); return 1; }
    if (strcmp(label, "DES")     == 0) { set_message(d, "DES: NOT IN USE"); return 1; }

    /* Row 1: DIR INTC | LEGS | DEP ARR | HOLD | PROG */
    if (strcmp(label, "DIR INTC") == 0) { set_message(d, "DIR: SELECT WPT"); return 1; }
    if (strcmp(label, "LEGS")    == 0) { d->current_page = 2; return 1; }
    if (strcmp(label, "DEP ARR") == 0) { set_message(d, "DEP/ARR: USE RTE"); return 1; }
    if (strcmp(label, "HOLD")    == 0) { set_message(d, "HOLD: NOT IN USE"); return 1; }
    if (strcmp(label, "PROG")    == 0) { d->current_page = 4; return 1; }

    /* Row 2: FIX | NAV RAD | PREV PAGE | NEXT PAGE | PERF */
    if (strcmp(label, "FIX")      == 0) { set_message(d, "FIX: NOT IN USE"); return 1; }
    if (strcmp(label, "NAV RAD")  == 0) { d->current_page = 5; return 1; }
    if (strcmp(label, "PREV PAGE") == 0) {
        d->current_page = (d->current_page + 5) % 6;  /* cycle backward */
        d->legs_scroll = 0;
        return 1;
    }
    if (strcmp(label, "NEXT PAGE") == 0) {
        d->current_page = (d->current_page + 1) % 6;  /* cycle forward */
        d->legs_scroll = 0;
        return 1;
    }
    if (strcmp(label, "PERF")    == 0) { d->current_page = 3; return 1; }

    /* EXEC */
    if (strcmp(label, "EXEC")    == 0) { exec_scratchpad(d); return 1; }

    /* === Edit keys (right column) === */
    if (strcmp(label, "CLR") == 0) {
        memset(d->scratchpad, 0, sizeof(d->scratchpad));
        d->message_scroll = 0;
        return 1;
    }
    if (strcmp(label, "DEL") == 0) {
        int slen = (int)strlen(d->scratchpad);
        if (slen > 0) d->scratchpad[slen - 1] = '\0';
        return 1;
    }
    if (strcmp(label, "SP") == 0) {
        int slen = (int)strlen(d->scratchpad);
        if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = ' '; d->scratchpad[slen + 1] = '\0'; }
        return 1;
    }
    if (strcmp(label, "/") == 0) {
        int slen = (int)strlen(d->scratchpad);
        if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = '/'; d->scratchpad[slen + 1] = '\0'; }
        return 1;
    }

    /* === Numbers & symbols === */
    if (strcmp(label, "+/-") == 0) {
        int slen = (int)strlen(d->scratchpad);
        if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = '-'; d->scratchpad[slen + 1] = '\0'; }
        return 1;
    }
    if (label[0] >= '0' && label[0] <= '9' && label[1] == '\0') {
        int slen = (int)strlen(d->scratchpad);
        if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = label[0]; d->scratchpad[slen + 1] = '\0'; }
        return 1;
    }
    if (strcmp(label, ".") == 0) {
        int slen = (int)strlen(d->scratchpad);
        if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = '.'; d->scratchpad[slen + 1] = '\0'; }
        return 1;
    }

    /* === Letters === */
    if (label[0] >= 'A' && label[0] <= 'Z' && label[1] == '\0') {
        int slen = (int)strlen(d->scratchpad);
        if (slen < MAX_SCRATCHPAD) { d->scratchpad[slen] = label[0]; d->scratchpad[slen + 1] = '\0'; }
        return 1;
    }

    return 0;
}

/**
 * @brief Check if a mouse click hit a CDU keypad button.
 *
 * Layout (B737 CDU style, 3 func rows + 6 alpha rows):
 *   Func R0: [INIT REF] [RTE] [CLB] [CRZ] [DES]
 *   Func R1: [DIR INTC] [LEGS] [DEP ARR] [HOLD] [PROG]
 *   Func R2: [FIX] [NAV RAD] [PREV PAGE] [NEXT PAGE] [PERF]    [EXEC]
 *   Alpha:   [nums ×3] | [letters ×5] | [edit ×1 wide]
 * @return 1 if handled, 0 if not.
 */
static int check_cdu_click(FMCData* d, const SDL_Rect* inst_rect, int mx, int my)
{
    /* Recompute layout (must match draw_fmc_screen) */
    int lsk_col_w  = LSK_BTN_W + LSK_GAP;
    int screen_w   = inst_rect->w - 2 * (SCREEN_MARGIN + lsk_col_w);
    if (screen_w < 120) screen_w = 120;
    int screen_h   = (int)((float)inst_rect->h * SCREEN_H_PCT / 100.0f);
    int screen_top = SCREEN_MARGIN + TITLE_BAR_H;

    int kb_y = screen_top + screen_h + KB_GAP;
    int kb_h = inst_rect->h - kb_y - KB_BOTTOM_GAP;
    if (kb_h < 80) kb_h = 80;
    int kb_x = SCREEN_MARGIN;
    int kb_w = inst_rect->w - 2 * SCREEN_MARGIN;  /* keyboard spans full width */

    /* Must be within keyboard area */
    if (my < kb_y || my >= kb_y + kb_h) return 0;
    if (mx < kb_x || mx >= kb_x + kb_w)  return 0;

    int pad = 3, margin = 4;
    int exec_rows = 1, func_rows = 3, alpha_rows = 6;
    int total_rows = exec_rows + func_rows + alpha_rows;
    int key_h = (kb_h - margin * 2 - pad * (total_rows - 1)) / total_rows;
    if (key_h < 16) key_h = 16;

    /* === EXEC row (right-aligned, between screen and func keys) === */
    {
        int exec_w = kb_w * 3 / 10;  /* ~30% of keyboard width */
        if (exec_w < 60) exec_w = 60;
        int exec_x = kb_x + kb_w - margin - exec_w;
        int exec_y = kb_y + margin;
        int exec_h = key_h;
        if (mx >= exec_x && mx < exec_x + exec_w
            && my >= exec_y && my < exec_y + exec_h)
            return cdu_button_action(d, "EXEC");
    }

    /* === Function key rows (3 rows × 5 cols) === */
    {
        int func_y = kb_y + margin + exec_rows * (key_h + pad);
        int fk_w = (kb_w - margin * 2 - pad * 4) / 5;
        if (fk_w < 28) fk_w = 28;

        const char* fkeys[3][5] = {
            { "INIT REF", "RTE",  "CLB", "CRZ", "DES"      },
            { "DIR INTC", "LEGS", "DEP ARR", "HOLD", "PROG"},
            { "FIX", "NAV RAD", "PREV PAGE", "NEXT PAGE", "PERF" }
        };

        for (int row = 0; row < func_rows; row++) {
            int by = func_y + row * (key_h + pad);
            if (my >= by && my < by + key_h) {
                for (int c = 0; c < 5; c++) {
                    int bx = kb_x + margin + c * (fk_w + pad);
                    if (mx >= bx && mx < bx + fk_w)
                        return cdu_button_action(d, fkeys[row][c]);
                }
                return 0;
            }
        }
    }

    /* === Alphanumeric section (6 rows) === */
    {
        int alpha_y = kb_y + margin + (exec_rows + func_rows) * (key_h + pad);
        int num_cols = 3, let_cols = 5, edit_cols = 1;
        int total_cols = num_cols + let_cols + edit_cols;
        int col_w = (kb_w - margin * 2 - pad * (total_cols + 1)) / total_cols;
        if (col_w < 18) col_w = 18;

        /* Number keys (left, 3 cols × 4 rows = 12 keys) */
        const char* nums[12] = {"1","2","3","4","5","6","7","8","9",".","0","+/-"};
        /* Letter keys (center, 5 cols × 6 rows = 30 positions, 26 used) */
        const char* lets[30] = {
            "A","B","C","D","E",  "F","G","H","I","J",
            "K","L","M","N","O",  "P","Q","R","S","T",
            "U","V","W","X","Y",  "Z","","","",""
        };
        /* Edit keys (right, 1 wide col × 4 rows) */
        const char* edits[4] = {"CLR","DEL","SP","/"};

        for (int row = 0; row < alpha_rows; row++) {
            int by = alpha_y + row * (key_h + pad);
            if (my < by || my >= by + key_h) continue;

            /* Number column group */
            if (row < 4) {
                for (int c = 0; c < num_cols; c++) {
                    int idx = row * num_cols + c;
                    int bx = kb_x + margin + c * (col_w + pad);
                    if (mx >= bx && mx < bx + col_w)
                        return cdu_button_action(d, nums[idx]);
                }
            }

            /* Letter column group */
            {
                int lx = kb_x + margin + num_cols * (col_w + pad) + pad;
                for (int c = 0; c < let_cols; c++) {
                    int idx = row * let_cols + c;
                    if (idx >= 30) continue;
                    const char* label = lets[idx];
                    if (!label[0]) continue;
                    int bx = lx + c * (col_w + pad);
                    if (mx >= bx && mx < bx + col_w)
                        return cdu_button_action(d, label);
                }
            }

            /* Edit column group */
            if (row < 4) {
                int ex = kb_x + margin + (num_cols + let_cols) * (col_w + pad) + pad;
                if (mx >= ex && mx < ex + col_w)
                    return cdu_button_action(d, edits[row]);
            }
        }
    }

    return 0;
}

/* =========================================================================
 *  Mouse hit-testing for LSK buttons
 * ========================================================================= */

/**
 * @brief Given mouse coordinates relative to the FMC instrument rect,
 *        determine if an LSK was clicked.
 * @return 0 if no LSK hit, 1 if handled.
 */
static int check_lsk_click(FMCData* d, const SDL_Rect* inst_rect, int mx, int my)
{
    (void)inst_rect;  /* mx,my already relative to instrument */

    /* CDU screen layout (matches draw_fmc_screen) */
    int lsk_col_w  = LSK_BTN_W + LSK_GAP;
    int screen_left = SCREEN_MARGIN + lsk_col_w;
    int screen_top  = SCREEN_MARGIN + TITLE_BAR_H;
    int screen_w    = inst_rect->w - 2 * (SCREEN_MARGIN + lsk_col_w);
    if (screen_w < 120) screen_w = 120;
    int screen_h    = (int)((float)inst_rect->h * SCREEN_H_PCT / 100.0f);
    int line_h      = screen_h / 12;

    /* LSK button positions */
    int lsk_left_x1  = SCREEN_MARGIN;
    int lsk_left_x2  = SCREEN_MARGIN + LSK_BTN_W;
    int lsk_right_x1 = screen_left + screen_w + LSK_GAP;
    int lsk_right_x2 = lsk_right_x1 + LSK_BTN_W;

    /* Determine which LSK line (0-5) was clicked */
    for (int line = 0; line < 6; line++) {
        int lsk_y1 = screen_top + line * 2 * line_h;
        int lsk_y2 = lsk_y1 + line_h * 2;

        if (my >= lsk_y1 && my <= lsk_y2) {
            if (mx >= lsk_left_x1 && mx <= lsk_left_x2) {
                handle_lsk(d, 0, line);
                return 1;
            }
            if (mx >= lsk_right_x1 && mx <= lsk_right_x2) {
                handle_lsk(d, 1, line);
                return 1;
            }
        }
    }

    return 0;
}

/* =========================================================================
 *  Rendering
 * ========================================================================= */

static void draw_fmc_screen(SDL_Renderer* r, const SDL_Rect* rect, FMCData* d)
{
    /* LSK button columns shrink the screen horizontally */
    int lsk_col_w  = LSK_BTN_W + LSK_GAP;  /* total column per side */
    int screen_left = rect->x + SCREEN_MARGIN + lsk_col_w;
    int screen_top  = rect->y + SCREEN_MARGIN + TITLE_BAR_H;
    int screen_w    = rect->w - 2 * (SCREEN_MARGIN + lsk_col_w);
    if (screen_w < 120) screen_w = 120;
    int screen_h    = (int)((float)rect->h * SCREEN_H_PCT / 100.0f);
    int line_h      = screen_h / 12;

    /* Cache layout for mouse hit-testing */
    d->screen_area.x = screen_left;
    d->screen_area.y = screen_top;
    d->screen_area.w = screen_w;
    d->screen_area.h = screen_h;
    d->line_h        = line_h;

    /* =================================================================
     *  LEFT LSK BUTTONS (L1–L6)
     * ================================================================= */
    {
        int lx = rect->x + SCREEN_MARGIN;
        for (int i = 0; i < 6; i++) {
            int ly = screen_top + i * 2 * line_h;
            int lh = line_h * 2 - 1;
            int lw = LSK_BTN_W;

            /* Button background */
            SDL_Rect btn = { lx, ly, lw, lh };
            set_col(r, COL_DARK_GRAY);
            SDL_RenderFillRect(r, &btn);
            set_col(r, COL_GRAY);
            SDL_RenderDrawRect(r, &btn);

            /* Label */
            char lbl[4];
            snprintf(lbl, sizeof(lbl), "L%d", i + 1);
            set_col(r, COL_AMBER);
            font_draw_scaled_aligned(r, lx + lw / 2, ly + lh / 2,
                                     lbl, 0.45f, FONT_BOLD, FONT_ALIGN_CENTER);

            /* Highlight if this LSK is "active" on current page */
            int highlight = 0;
            if (d->current_page == 1) { /* RTE page */
                if ((i == 0) || (i == 1) || (i == 5)) highlight = 1;
            }
            if (highlight) {
                set_col(r, COL_AMBER);
                SDL_RenderDrawRect(r, &btn); /* extra border */
            }
        }
    }

    /* =================================================================
     *  RIGHT LSK BUTTONS (R1–R6)
     * ================================================================= */
    {
        int rx = screen_left + screen_w + LSK_GAP;
        for (int i = 0; i < 6; i++) {
            int ry = screen_top + i * 2 * line_h;
            int rh = line_h * 2 - 1;
            int rw = LSK_BTN_W;

            SDL_Rect btn = { rx, ry, rw, rh };
            set_col(r, COL_DARK_GRAY);
            SDL_RenderFillRect(r, &btn);
            set_col(r, COL_GRAY);
            SDL_RenderDrawRect(r, &btn);

            char lbl[4];
            snprintf(lbl, sizeof(lbl), "R%d", i + 1);
            set_col(r, COL_AMBER);
            font_draw_scaled_aligned(r, rx + rw / 2, ry + rh / 2,
                                     lbl, 0.45f, FONT_BOLD, FONT_ALIGN_CENTER);

            /* Highlight R1, R2, R6 on RTE page */
            int highlight = 0;
            if (d->current_page == 1) { /* RTE page */
                if ((i == 0) || (i == 1) || (i == 5)) highlight = 1;
            }
            if (highlight) {
                set_col(r, COL_AMBER);
                SDL_RenderDrawRect(r, &btn);
            }
        }
    }

    /* Screen background */
    set_col(r, COL_SCREEN_BG);
    SDL_Rect screen_bg = { screen_left, screen_top, screen_w, screen_h };
    SDL_RenderFillRect(r, &screen_bg);
    set_col(r, COL_GRAY);
    SDL_RenderDrawRect(r, &screen_bg);

    /* Title bar */
    set_col(r, COL_TAPE_BG);
    SDL_Rect title_bg = { screen_left, screen_top - TITLE_BAR_H, screen_w, TITLE_BAR_H };
    SDL_RenderFillRect(r, &title_bg);
    set_col(r, COL_WHITE);
    draw_text_simple(r, screen_left + screen_w / 2, screen_top - TITLE_BAR_H / 2, "FMC-CDU", 0.8f);

    /* Page indicator */
    const char* page_names[] = { "IDENT", "RTE", "LEGS", "PERF", "PROG", "RADIO" };
    char page_title[32];
    snprintf(page_title, sizeof(page_title), "%d/6 %s",
             d->current_page + 1, page_names[d->current_page]);
    draw_text_simple(r, screen_left + screen_w - 40, screen_top - TITLE_BAR_H / 2, page_title, 0.6f);

    /* Display lines */
    for (int ln = 0; ln < 12; ln++) {
        int y = screen_top + ln * line_h + line_h / 2;

        /* Display text (monospaced, left-aligned for column alignment) */
        set_col(r, COL_WHITE);
        int text_x = screen_left + 4;
        int text_y = y;
        font_draw_scaled_aligned(r, text_x, text_y, d->display[ln],
                                 0.52f, FONT_MONO, FONT_ALIGN_LEFT);
    }

    /* Scratchpad area (bottom 2 lines of CDU screen) */
    int scratch_line_y = screen_top + screen_h - line_h * 2;
    set_col(r, COL_TAPE_BG);
    SDL_Rect scratch_bg = { screen_left, scratch_line_y, screen_w, line_h * 2 };
    SDL_RenderFillRect(r, &scratch_bg);
    set_col(r, COL_DARK_GRAY);
    SDL_RenderDrawRect(r, &scratch_bg);

    /* Scratchpad label (left-aligned so it's fully visible) */
    set_col(r, COL_GRAY);
    font_draw_scaled_aligned(r, screen_left + 4, scratch_line_y + line_h / 2,
                             "SCRATCHPAD", 0.5f, FONT_REGULAR, FONT_ALIGN_LEFT);

    /* Scratchpad content (blinking cursor) */
    {
        char sp_display[32];
        int blink = ((SDL_GetTicks() / 500) % 2);  /* 500ms blink */
        snprintf(sp_display, sizeof(sp_display), "%s%c",
                 d->scratchpad, blink ? '_' : ' ');
        set_col(r, COL_CYAN);
        font_draw_scaled_aligned(r, screen_left + 4, scratch_line_y + line_h + line_h / 2,
                                 sp_display, 0.7f, FONT_MONO, FONT_ALIGN_LEFT);
    }

    /* Status message area (from linked list history) */
    {
        int msg_y = scratch_line_y + line_h + 4;
        int total_msgs = d->message_history ? ll_size(d->message_history) : 0;

        if (total_msgs > 0) {
            /* message_scroll: 0 = latest, N = Nth older */
            int idx = total_msgs - 1 - d->message_scroll;
            if (idx < 0) idx = 0;

            FmcMessage* m = (FmcMessage*)ll_get(d->message_history, idx);
            if (m) {
                int show = (d->message_scroll > 0) ||
                           ((SDL_GetTicks() - m->timestamp) < MSG_TIMEOUT_MS);
                if (show) {
                    set_col(r, COL_AMBER);
                    draw_text_simple(r, screen_left + screen_w / 2, msg_y,
                                     m->text, 0.6f);

                    /* Scroll indicator (when viewing history) */
                    if (d->message_scroll > 0) {
                        char indicator[24];
                        snprintf(indicator, sizeof(indicator), "MSG %d/%d",
                                 d->message_scroll + 1, total_msgs);
                        set_col(r, COL_GRAY);
                        draw_text_simple(r, screen_left + screen_w - 30,
                                         msg_y, indicator, 0.4f);
                    }
                }
            }
        }
    }

    /* Border */
    // set_col(r, COL_GRAY);
    // SDL_RenderDrawRect(r, rect);
}

/* =========================================================================
 *  Instrument vtable
 * ========================================================================= */

static void fmc_on_init(Instrument* self, App* app)
{
    FMCData* d = (FMCData*)self->private_data;
    d->fmc          = app->fmc_state;
    d->app          = app;
    d->current_page   = 0;
    d->legs_scroll    = 0;
    d->message_scroll = 0;
    memset(d->scratchpad, 0, sizeof(d->scratchpad));
    memset(d->origin_input, 0, sizeof(d->origin_input));
    memset(d->dest_input,   0, sizeof(d->dest_input));

    /* Create message history linked list */
    d->message_history = ll_create();

    /* Build route graph from nav data */
    if (d->fmc) {
        d->graph = route_graph_build(d->fmc);
        if (d->graph) {
            set_message(d, "NAV DB LOADED");
        } else {
            set_message(d, "WARN: NO GRAPH");
        }
    }

    /* Create AVL tree for sorted waypoint display */
    d->wpt_tree = avl_create(wpt_compare_ident, NULL);

    /* Load background image and parse layout */
    SDL_Surface* surf = IMG_Load("assets/assets/fmc.png");
    if (surf) {
        d->bg_texture = SDL_CreateTextureFromSurface(app->renderer, surf);
        SDL_FreeSurface(surf);
    } else {
        LOG_WARN("Failed to load assets/assets/fmc.png in fmc.c");
    }
    parse_json_layout(d);

    build_page(d);
    LOG_INFO("FMC initialized (graph: %s, avl: %s, msgs: %s)",
             d->graph ? "OK" : "NONE",
             d->wpt_tree ? "OK" : "FAIL",
             d->message_history ? "LL" : "FAIL");
}

static void fmc_on_update(Instrument* self, const FlightData* fd, float dt)
{
    (void)dt;
    FMCData* d = (FMCData*)self->private_data;
    if (!fd) return;

    FlightDataValues snapshot;
    flight_data_snapshot((FlightData*)fd, &snapshot);
    d->smooth_gs  = exp_smooth(d->smooth_gs,  snapshot.gs_kts, 0.1f);
    d->smooth_alt = exp_smooth(d->smooth_alt, snapshot.altitude_ft, 0.1f);

    /* Rebuild current page (live data) */
    build_page(d);
}

static void fmc_on_render(Instrument* self, SDL_Renderer* renderer)
{
    FMCData* d = (FMCData*)self->private_data;
    const SDL_Rect* rect = &self->rect;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    if (d->bg_texture) {
        SDL_RenderCopy(renderer, d->bg_texture, NULL, rect);
    }

    int sx = (int)(d->screen_bbox.x1 * rect->w);
    int sy = (int)(d->screen_bbox.y1 * rect->h);
    int sw = (int)((d->screen_bbox.x2 - d->screen_bbox.x1) * rect->w);
    int sh = (int)((d->screen_bbox.y2 - d->screen_bbox.y1) * rect->h);

    int line_h = sh / 14; // Slightly more lines to fit text nicely
    int screen_top = sy + 4;
    int screen_left = sx + 4;

    // Title bar
    set_col(renderer, COL_WHITE);
    const char* page_names[] = { "IDENT", "RTE", "LEGS", "PERF", "PROG", "RADIO" };
    char page_title[32];
    snprintf(page_title, sizeof(page_title), "%d/6 %s", d->current_page + 1, page_names[d->current_page]);
    draw_text_simple(renderer, sx + sw - 40, screen_top + line_h/2, page_title, 0.6f);
    draw_text_simple(renderer, sx + sw/2, screen_top + line_h/2, "FMC-CDU", 0.7f);

    /* Display lines */
    for (int ln = 0; ln < 12; ln++) {
        int y = screen_top + (ln + 1) * line_h + line_h / 2;

        /* Display text (monospaced, left-aligned for column alignment) */
        set_col(renderer, COL_WHITE);
        font_draw_scaled_aligned(renderer, screen_left, y, d->display[ln],
                                 0.55f, FONT_MONO, FONT_ALIGN_LEFT);
    }

    /* Scratchpad content */
    int scratch_line_y = screen_top + 13 * line_h;
    {
        char sp_display[32];
        int blink = ((SDL_GetTicks() / 500) % 2);  /* 500ms blink */
        snprintf(sp_display, sizeof(sp_display), "%s%c",
                 d->scratchpad, blink ? '_' : ' ');
        set_col(renderer, COL_CYAN);
        font_draw_scaled_aligned(renderer, screen_left, scratch_line_y,
                                 sp_display, 0.65f, FONT_MONO, FONT_ALIGN_LEFT);
    }

    /* Status message area (from linked list history) */
    {
        int msg_y = scratch_line_y - line_h;
        int total_msgs = d->message_history ? ll_size(d->message_history) : 0;

        if (total_msgs > 0) {
            /* message_scroll: 0 = latest, N = Nth older */
            int idx = total_msgs - 1 - d->message_scroll;
            if (idx < 0) idx = 0;

            FmcMessage* m = (FmcMessage*)ll_get(d->message_history, idx);
            if (m) {
                int show = (d->message_scroll > 0) ||
                           ((SDL_GetTicks() - m->timestamp) < MSG_TIMEOUT_MS);
                if (show) {
                    set_col(renderer, COL_AMBER);
                    draw_text_simple(renderer, sx + sw / 2, msg_y, m->text, 0.6f);
                }
            }
        }
    }
}

static int fmc_on_event(Instrument* self, const SDL_Event* ev)
{
    FMCData* d = (FMCData*)self->private_data;

    /* --- Mouse: LSK & CDU button clicks --- */
    if (ev->type == SDL_MOUSEBUTTONDOWN) {
        int mx = ev->button.x - self->rect.x;
        int my = ev->button.y - self->rect.y;

        /* Check if within this instrument's rect */
        if (mx >= 0 && my >= 0 && mx < self->rect.w && my < self->rect.h) {
            for (int i = 0; i < d->button_count; i++) {
                int bx = (int)(d->buttons[i].x1 * self->rect.w);
                int by = (int)(d->buttons[i].y1 * self->rect.h);
                int bw = (int)((d->buttons[i].x2 - d->buttons[i].x1) * self->rect.w);
                int bh = (int)((d->buttons[i].y2 - d->buttons[i].y1) * self->rect.h);

                if (mx >= bx && mx <= bx + bw && my >= by && my <= by + bh) {
                    const char* label = d->buttons[i].label;
                    if (strncmp(label, "LSK_", 4) == 0) {
                        int num = label[4] - '1';
                        if (label[5] == 'L') handle_lsk(d, 0, num);
                        else if (label[5] == 'R') handle_lsk(d, 1, num);
                    } else {
                        cdu_button_action(d, label);
                    }
                    return 1;
                }
            }
        }
    }

    /* --- Keyboard (fallback: F1-F6 page nav, Enter/Del, or type when mouse over FMC) --- */
    if (ev->type == SDL_KEYDOWN) {
        SDL_Keycode key = ev->key.keysym.sym;

        /* Page navigation (F1-F5) */
        if (key == SDLK_F1) { d->current_page = 0; d->legs_scroll = 0; return 1; }
        if (key == SDLK_F2) { d->current_page = 1; d->legs_scroll = 0; return 1; }
        if (key == SDLK_F3) { d->current_page = 2; return 1; }
        if (key == SDLK_F4) { d->current_page = 3; return 1; }
        if (key == SDLK_F5) { d->current_page = 4; return 1; }
        if (key == SDLK_F7) { d->current_page = 5; return 1; }

        /* F6 = toggle LEGS sort mode */
        if (key == SDLK_F6) {
            d->legs_sort_mode = !d->legs_sort_mode;
            d->legs_scroll = 0;
            set_message(d, d->legs_sort_mode
                        ? "LEGS: ALPHABETICAL" : "LEGS: ROUTE ORDER");
            return 1;
        }

        /* Tab = cycle pages forward */
        if (key == SDLK_TAB) {
            d->current_page = (d->current_page + 1) % 6;
            d->legs_scroll  = 0;
            return 1;
        }

        /* Page Up/Down for LEGS scrolling (page 2 only) */
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

        /* Page Up/Down for message history scrolling (non-LEGS pages) */
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

        /* Scratchpad editing */
        int slen = (int)strlen(d->scratchpad);

        if (key == SDLK_BACKSPACE || key == SDLK_DELETE) {
            if (slen > 0) d->scratchpad[slen - 1] = '\0';
            /* Forward DEL to X-Plane FMC */
            xp_forward_cdu_key(d, "DEL");
            return 1;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            exec_scratchpad(d);
            /* Forward EXEC to X-Plane FMC */
            xp_forward_cdu_key(d, "EXEC");
            return 1;
        }
        if (key == SDLK_ESCAPE) {
            memset(d->scratchpad, 0, sizeof(d->scratchpad));
            d->message_scroll = 0;
            /* Forward CLR to X-Plane FMC */
            xp_forward_cdu_key(d, "CLR");
            return 1;
        }

        /* Alphanumeric entry */
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
            /* Forward alphanumeric/symbol to X-Plane FMC */
            {
                char lbl[2] = {ch, '\0'};
                if (ch == ' ')      xp_forward_cdu_key(d, "SP");
                else if (ch == '/') xp_forward_cdu_key(d, "/");
                else if (ch == '.') xp_forward_cdu_key(d, ".");
                else if (ch == '-') xp_forward_cdu_key(d, "+/-");
                else                xp_forward_cdu_key(d, lbl);
            }
            return 1;
        }
    }

    return 0;  /* Not consumed */
}

static void fmc_on_destroy(Instrument* self)
{
    if (self && self->private_data) {
        FMCData* d = (FMCData*)self->private_data;
        if (d->graph) {
            route_graph_destroy(d->graph);
            d->graph = NULL;
        }
        if (d->wpt_tree) {
            avl_destroy(d->wpt_tree, 0);  /* don't free waypoints */
            d->wpt_tree = NULL;
        }
        if (d->message_history) {
            ll_destroy(d->message_history, 1);  /* free FmcMessage nodes */
            d->message_history = NULL;
        }
        if (d->bg_texture) {
            SDL_DestroyTexture(d->bg_texture);
            d->bg_texture = NULL;
        }
        free(self->private_data);
        self->private_data = NULL;
    }
}

/* =========================================================================
 *  Constructor
 * ========================================================================= */

Instrument* fmc_create(void)
{
    Instrument* inst = calloc(1, sizeof(Instrument));
    if (!inst) return NULL;

    FMCData* data = calloc(1, sizeof(FMCData));
    if (!data) { free(inst); return NULL; }

    data->current_page    = 0;
    data->legs_scroll     = 0;
    data->message_scroll  = 0;
    data->graph           = NULL;
    data->fmc             = NULL;
    data->message_history = NULL;
    data->smooth_gs       = 0.0f;
    data->smooth_alt      = 0.0f;
    memset(data->scratchpad, 0, sizeof(data->scratchpad));
    memset(data->display, ' ', sizeof(data->display));

    inst->name         = "FMC";
    inst->on_init      = fmc_on_init;
    inst->on_update    = fmc_on_update;
    inst->on_render    = fmc_on_render;
    inst->on_event     = fmc_on_event;
    inst->on_destroy   = fmc_on_destroy;
    inst->private_data = data;

    return inst;
}
