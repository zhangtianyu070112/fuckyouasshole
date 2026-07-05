/**
 * @file    route_graph.c
 * @brief   Airway route graph builder + A* shortest-path engine.
 *
 * Implementation overview:
 *   1. Graph construction: walk FMCState nav database, create nodes for each
 *      waypoint/airport/navaid, then add edges by matching airway designators
 *      and proximity-based fallback connections.
 *   2. A* search: uses a binary min-heap for the open set and generational
 *      visit tokens (no per-query memset). Heuristic = great-circle distance
 *      to the goal — admissible (never overestimates) on a sphere.
 *   3. Path reconstruction: follows came_from[] back from goal to start,
 *      reverses, and fills a RoutePath.
 */

#include "route_graph.h"
#include "ds/hash_table.h"
#include "utils/logger.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 *  Internal constants
 * ========================================================================= */

#define INF_SCORE       (1e18f)

/* =========================================================================
 *  Internal: min-heap (keyed by f_score)
 * ========================================================================= */

static void heap_swap(RouteGraph* g, int i, int j)
{
    int tmp = g->heap[i];
    g->heap[i] = g->heap[j];
    g->heap[j] = tmp;
    g->heap_pos[g->heap[i]] = i;
    g->heap_pos[g->heap[j]] = j;
}

static void heap_sift_up(RouteGraph* g, int idx)
{
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (g->heap_key[g->heap[idx]] < g->heap_key[g->heap[parent]]) {
            heap_swap(g, idx, parent);
            idx = parent;
        } else {
            break;
        }
    }
}

static void heap_sift_down(RouteGraph* g, int idx)
{
    int size = g->heap_size;
    while (1) {
        int smallest = idx;
        int left  = 2 * idx + 1;
        int right = 2 * idx + 2;

        if (left < size && g->heap_key[g->heap[left]] < g->heap_key[g->heap[smallest]]) {
            smallest = left;
        }
        if (right < size && g->heap_key[g->heap[right]] < g->heap_key[g->heap[smallest]]) {
            smallest = right;
        }
        if (smallest != idx) {
            heap_swap(g, idx, smallest);
            idx = smallest;
        } else {
            break;
        }
    }
}

static void heap_push(RouteGraph* g, int node_index, float key)
{
    if (g->heap_pos[node_index] >= 0) {
        /* Already in heap — update key */
        float old_key = g->heap_key[node_index];
        g->heap_key[node_index] = key;
        if (key < old_key) {
            heap_sift_up(g, g->heap_pos[node_index]);
        } else if (key > old_key) {
            heap_sift_down(g, g->heap_pos[node_index]);
        }
        return;
    }

    int pos = g->heap_size;
    g->heap[pos]           = node_index;
    g->heap_key[node_index] = key;
    g->heap_pos[node_index] = pos;
    g->heap_size++;
    heap_sift_up(g, pos);
}

static int heap_pop(RouteGraph* g)
{
    if (g->heap_size <= 0) return -1;
    int result = g->heap[0];
    g->heap_pos[result] = -1;

    g->heap_size--;
    if (g->heap_size > 0) {
        g->heap[0] = g->heap[g->heap_size];
        g->heap_pos[g->heap[0]] = 0;
        heap_sift_down(g, 0);
    }
    return result;
}

static void heap_clear(RouteGraph* g)
{
    g->heap_size = 0;
}

/* =========================================================================
 *  Internal: node lookup
 * ========================================================================= */

/**
 * @brief O(1) node lookup via hash table. Returns -1 if not found.
 */
static int find_node(const RouteGraph* g, const char* ident)
{
    if (!g->node_lookup) return -1;
    int* idx = (int*)ht_get((HashTable*)g->node_lookup, ident);
    return idx ? *idx : -1;
}

/* =========================================================================
 *  Internal: edge management
 * ========================================================================= */

/**
 * @brief Add a directed edge from node u to node v.
 */
static void add_edge(RouteGraph* g, int from, int to, float dist_nm,
                     const char* airway, int min_alt, int max_alt)
{
    if (from < 0 || from >= g->node_count) return;
    if (to < 0   || to   >= g->node_count) return;

    RouteEdge* e = calloc(1, sizeof(RouteEdge));
    if (!e) {
        LOG_WARN("Out of memory adding edge %d→%d", from, to);
        return;
    }
    e->to_index    = to;
    e->distance_nm = dist_nm;
    if (airway) {
        strncpy(e->airway, airway, sizeof(e->airway) - 1);
    }
    e->min_alt_ft = min_alt;
    e->max_alt_ft = (max_alt <= 0) ? 99999 : max_alt;

    /* Prepend to adjacency list */
    e->next = g->nodes[from].edges;
    g->nodes[from].edges = e;
    g->nodes[from].edge_count++;
}

/**
 * @brief Add a bidirectional edge.
 */
static void add_edge_bidi(RouteGraph* g, int a, int b, float dist_nm,
                          const char* airway, int min_alt, int max_alt)
{
    add_edge(g, a, b, dist_nm, airway, min_alt, max_alt);
    add_edge(g, b, a, dist_nm, airway, min_alt, max_alt);
}

/* =========================================================================
 *  Graph construction
 * ========================================================================= */

/**
 * @brief Add a waypoint or airport as a graph node.
 */
static int add_node(RouteGraph* g, const char* ident, WaypointType type,
                    GeoPos pos, float elevation_ft)
{
    if (g->node_count >= ROUTE_GRAPH_MAX_NODES) {
        LOG_WARN("RouteGraph: max nodes (%d) reached", ROUTE_GRAPH_MAX_NODES);
        return -1;
    }
    /* O(1) duplicate check via hash table */
    if (ht_contains((HashTable*)g->node_lookup, ident)) {
        int* idx = (int*)ht_get((HashTable*)g->node_lookup, ident);
        return idx ? *idx : -1;
    }

    int idx = g->node_count++;
    memset(&g->nodes[idx], 0, sizeof(RouteNode));
    strncpy(g->nodes[idx].ident, ident, sizeof(g->nodes[idx].ident) - 1);
    g->nodes[idx].type         = type;
    g->nodes[idx].pos          = pos;
    g->nodes[idx].elevation_ft = elevation_ft;
    g->nodes[idx].edges        = NULL;
    g->nodes[idx].edge_count   = 0;
    g->nodes[idx].is_portal    = 0;

    /* Register in hash table: ident → index */
    if (g->node_lookup) {
        int* p_idx = (int*)malloc(sizeof(int));
        if (p_idx) { *p_idx = idx; ht_put((HashTable*)g->node_lookup, ident, p_idx); }
    }

    return idx;
}

/**
 * @brief Build the full route graph from FMCState nav database.
 */
RouteGraph* route_graph_build(const FMCState* state)
{
    if (!state) return NULL;

    RouteGraph* g = calloc(1, sizeof(RouteGraph));
    if (!g) {
        LOG_ERROR("Out of memory allocating RouteGraph");
        return NULL;
    }

    /* Init heap positions to -1 (not in heap) */
    for (int i = 0; i < ROUTE_GRAPH_MAX_NODES; i++) {
        g->heap_pos[i]  = -1;
        g->g_score[i]   = (float)INF_SCORE;
        g->f_score[i]   = (float)INF_SCORE;
        g->came_from[i] = -1;
    }
    g->heap_size   = 0;
    g->visit_token = 0;

    /* Create hash table for O(1) node lookup */
    g->node_lookup = ht_create(211);

    /* --- Phase 1: Add nodes --- */

    /* Airports */
    for (int i = 0; i < state->nav_apt_count && i < 128; i++) {
        const Airport* apt = &state->nav_airports[i];
        add_node(g, apt->icao, WPT_AIRPORT, apt->pos, apt->elevation_ft);
    }

    /* Waypoints & navaids */
    for (int i = 0; i < state->nav_wpt_count && i < 512; i++) {
        const Waypoint* wpt = &state->nav_waypoints[i];
        add_node(g, wpt->ident, wpt->type, wpt->pos, wpt->elevation_ft);
    }

    LOG_INFO("RouteGraph: %d nodes created (airports + waypoints + navaids)",
             g->node_count);

    /* --- Phase 2: Airway-based edge construction --- */

    typedef struct {
        const char* airway;
        const char* from_wpt;
        const char* to_wpt;
        int    min_alt_ft;
        int    max_alt_ft;
    } AirwaySeg;

    static const AirwaySeg AIRWAYS[] = {
        /* A593: Beijing coastal route → Shanghai */
        { "A593", "PILOS", "LADIX",  9800, 41000 },
        { "A593", "LADIX", "LJG",    9800, 41000 },
        { "A593", "LJG",   "LAMEN",  9800, 41000 },
        { "A593", "LAMEN", "DUOBA",  9800, 41000 },
        { "A593", "DUOBA", "AND",    9800, 41000 },
        /* A326: Beijing inland → Shanghai */
        { "A326", "PANKI", "SUGOL",  9800, 39000 },
        { "A326", "SUGOL", "UDINO",  9800, 39000 },
        /* A470: Shanghai south → Guangzhou */
        { "A470", "LUPVI", "SAVOK",  9800, 37100 },
        { "A470", "SAVOK", "FQG",    9800, 37100 },
        { "A470", "FQG",   "BUPAN",  9800, 37100 },
        { "A470", "BUPAN", "DOTIO",  9800, 37100 },
        { "A470", "DOTIO", "BIGRO",  9800, 37100 },
        /* W51: Chengdu → Beijing */
        { "W51",  "IDUXA", "PANKI",  9800, 41000 },
        /* Cross-airway connections */
        { "A593", "BOBAK", "PILOS",  9800, 41000 },
        { "A326", "BOBAK", "PANKI",  9800, 39000 },
        { "A470", "AND",   "LUPVI",  9800, 37100 },
        /* SID/STAR: airport ↔ airway entry */
        { "SID",  "ZBAA", "PILOS",      0, 10000 },
        { "SID",  "ZBAA", "BOBAK",      0, 10000 },
        { "STAR", "AND",  "ZSSS",       0, 10000 },
        { "STAR", "UDINO","ZSSS",       0, 10000 },
        { "STAR", "AND",  "ZSPD",       0, 10000 },
        { "STAR", "BIGRO","ZGGG",       0, 10000 },
        { "SID",  "ZBTJ", "BOBAK",      0, 10000 },
        { "STAR", "BIGRO","ZSAM",       0, 10000 },
        { "SID",  "ZUUU", "IDUXA",      0, 12000 },
        { "STAR", "UDINO","ZSHC",       0, 10000 },
        { "STAR", "UDINO","ZSNJ",       0, 10000 },
        { "STAR", "FQG",  "ZSAM",       0, 10000 },
    };
    int airway_count = (int)(sizeof(AIRWAYS) / sizeof(AIRWAYS[0]));

    for (int k = 0; k < airway_count; k++) {
        int from = find_node(g, AIRWAYS[k].from_wpt);
        int to   = find_node(g, AIRWAYS[k].to_wpt);
        if (from < 0 || to < 0) continue;

        float dist = (float)geo_distance_nm(g->nodes[from].pos,
                                            g->nodes[to].pos);
        add_edge_bidi(g, from, to, dist,
                      AIRWAYS[k].airway,
                      AIRWAYS[k].min_alt_ft,
                      AIRWAYS[k].max_alt_ft);
    }

    LOG_INFO("RouteGraph: %d airway segments processed", airway_count);

    /* --- Phase 3: Fallback — connect orphan nodes (< 2 edges) to
     *              nearest neighbors within 200 NM. --- */
    for (int i = 0; i < g->node_count; i++) {
        if (g->nodes[i].edge_count >= 2) continue;

        int    best1 = -1, best2 = -1;
        float  dist1 = 1e9f,  dist2 = 1e9f;

        for (int j = 0; j < g->node_count; j++) {
            if (i == j) continue;
            float d = (float)geo_distance_nm(g->nodes[i].pos,
                                             g->nodes[j].pos);
            if (d > 200.0f) continue;
            if (d < dist1) {
                dist2 = dist1; best2 = best1;
                dist1 = d;     best1 = j;
            } else if (d < dist2) {
                dist2 = d;     best2 = j;
            }
        }

        if (best1 >= 0) add_edge_bidi(g, i, best1, dist1, "DIRECT", 0, 99999);
        if (best2 >= 0) add_edge_bidi(g, i, best2, dist2, "DIRECT", 0, 99999);
    }

    int total_edges = 0;
    for (int i = 0; i < g->node_count; i++) {
        total_edges += g->nodes[i].edge_count;
    }

    LOG_INFO("RouteGraph: build complete — %d nodes, %d directed edges",
             g->node_count, total_edges);
    return g;
}

void route_graph_destroy(RouteGraph* g)
{
    if (!g) return;
    for (int i = 0; i < g->node_count; i++) {
        RouteEdge* e = g->nodes[i].edges;
        while (e) {
            RouteEdge* next = e->next;
            free(e);
            e = next;
        }
    }
    if (g->node_lookup) {
        ht_destroy((HashTable*)g->node_lookup, 1);  /* free_values=1 for int* indices */
    }
    free(g);
}

/* =========================================================================
 *  A* search
 * ========================================================================= */

/**
 * @brief Heuristic: great-circle distance from node to goal (NM).
 *        This is admissible — the shortest possible path on a sphere
 *        is the great-circle arc, so it never overestimates real distance.
 */
static float heuristic(const RouteNode* node, const RouteNode* goal)
{
    return (float)geo_distance_nm(node->pos, goal->pos);
}

/**
 * @brief Check if an edge is usable at the given cruise altitude.
 */
static int edge_altitude_ok(const RouteEdge* e, float cruise_alt_ft)
{
    /* Direct legs (no airway) are always OK */
    if (e->airway[0] == '\0') return 1;
    /* SID/STAR connections are always OK */
    if (strcmp(e->airway, "SID/STAR") == 0) return 1;
    /* Check altitude constraints */
    if (cruise_alt_ft < (float)e->min_alt_ft && e->min_alt_ft > 0) return 0;
    if (cruise_alt_ft > (float)e->max_alt_ft && e->max_alt_ft < 90000) return 0;
    return 1;
}

int route_graph_find_route(RouteGraph* g,
                           const char* origin_icao,
                           const char* dest_icao,
                           float cruise_alt_ft,
                           RoutePath* path)
{
    if (!g || !origin_icao || !dest_icao || !path) return 0;

    memset(path, 0, sizeof(*path));

    int start_idx = find_node(g, origin_icao);
    int goal_idx  = find_node(g, dest_icao);

    if (start_idx < 0) {
        LOG_WARN("A*: origin '%s' not found in graph", origin_icao);
        return 0;
    }
    if (goal_idx < 0) {
        LOG_WARN("A*: destination '%s' not found in graph", dest_icao);
        return 0;
    }

    /* --- Init search state --- */
    g->visit_token++;
    heap_clear(g);

    /* Reset g/f scores from previous queries.
     * Only need to reset nodes reachable by this query — but 1024 floats is
     * negligible. We use INF_SCORE as the unvisited sentinel. */
    for (int i = 0; i < g->node_count; i++) {
        g->g_score[i] = (float)INF_SCORE;
        g->f_score[i] = (float)INF_SCORE;
    }

    g->g_score[start_idx]   = 0.0f;
    g->f_score[start_idx]   = heuristic(&g->nodes[start_idx], &g->nodes[goal_idx]);
    g->came_from[start_idx] = -1;

    heap_push(g, start_idx, g->f_score[start_idx]);

    int explored = 0;

    /* --- Main A* loop --- */
    while (g->heap_size > 0) {
        int current = heap_pop(g);
        explored++;

        if (current == goal_idx) {
            /* --- Path found! Reconstruct --- */
            /* Count steps */
            int steps = 0;
            int node  = goal_idx;
            while (node >= 0 && steps < ROUTE_PATH_MAX_WP) {
                steps++;
                node = g->came_from[node];
            }

            /* Reverse into waypoints array */
            path->waypoint_count = steps;
            node = goal_idx;
            for (int i = steps - 1; i >= 0; i--) {
                RouteNode* rn = &g->nodes[node];
                memset(&path->waypoints[i], 0, sizeof(Waypoint));
                strncpy(path->waypoints[i].ident, rn->ident,
                        sizeof(path->waypoints[i].ident) - 1);
                path->waypoints[i].type  = rn->type;
                path->waypoints[i].pos   = rn->pos;
                path->waypoints[i].elevation_ft = rn->elevation_ft;
                node = g->came_from[node];
            }

            path->found              = 1;
            path->total_distance_nm  = g->g_score[goal_idx];

            /* Estimate time (hours) at cruise speed */
            {
                float avg_speed = 450.0f; /* kts, typical cruise */
                path->estimated_time_hours = path->total_distance_nm / avg_speed;
            }

            LOG_INFO("A*: route found! %d waypoints, %.0f NM, explored %d nodes",
                     path->waypoint_count, (double)path->total_distance_nm,
                     explored);
            return 1;
        }

        g->closed_set[current] = g->visit_token;

        /* --- Expand neighbors --- */
        for (RouteEdge* e = g->nodes[current].edges; e; e = e->next) {
            int neighbor = e->to_index;

            /* Skip if already visited this generation */
            if (g->closed_set[neighbor] == g->visit_token) continue;

            /* Altitude filtering */
            if (!edge_altitude_ok(e, cruise_alt_ft)) continue;

            float tentative_g = g->g_score[current] + e->distance_nm;

            if (tentative_g < g->g_score[neighbor]) {
                g->came_from[neighbor] = current;
                g->g_score[neighbor]   = tentative_g;
                g->f_score[neighbor]   = tentative_g
                    + heuristic(&g->nodes[neighbor], &g->nodes[goal_idx]);

                heap_push(g, neighbor, g->f_score[neighbor]);
            }
        }
    }

    /* No path found */
    LOG_WARN("A*: no route from %s to %s (explored %d nodes)",
             origin_icao, dest_icao, explored);
    path->found = 0;
    return 0;
}

/* =========================================================================
 *  Utility
 * ========================================================================= */

void route_path_free(RoutePath* path)
{
    if (path) memset(path, 0, sizeof(*path));
}

void route_graph_stats(RouteGraph* g, int* nodes_explored)
{
    if (g && nodes_explored) {
        *nodes_explored = g->visit_token;  /* proxy: generation counter */
    }
}
